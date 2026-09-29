"""Functional and protocol audit for mixed-traffic vector applications."""
from collections import Counter, defaultdict, deque
import csv
import importlib.util
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
_common_path = HERE / 'service_validation.py'
_spec = importlib.util.spec_from_file_location('application_transfer_common', _common_path)
_common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_common)
BASE, INPUT_X, INPUT_Y = 0x90000000, 0x90100000, 0x90200000
OUTPUT, WARM, MAILBOX = 0x90300000, 0x90400000, 0x90500000
FIELDS = ('pc', 'address', 'bytes', 'write', 'vector')
APPLICATIONS = ('copy', 'add', 'checksum', 'stencil')
IMPLEMENTATIONS = ('llvm',)
VLENS, BANKS, DEPTHS = (256, 1024), (8,), (1, 4)
MASK = 0xffffffff


def input_x(index):
    return ((index * 0x9e3779b1) ^ 0x13579bdf) & MASK


def input_y(index):
    return (index * 0x85ebca6b + 0x2468ace0) & MASK


def _rows(path):
    with path.open() as stream:
        yield from csv.DictReader(stream)


def _inside(cycle, phase):
    return phase['start_cycle'] <= cycle <= phase['end_cycle']


def _fragments(address, size, width):
    while size:
        part = min(size, width - address % width)
        yield address, part
        address += part
        size -= part


def _memory(trial, name, phases, cpu):
    pending = defaultdict(deque)
    totals, counts = Counter(), {key: Counter() for key in phases}
    accesses = {key: [] for key in phases}
    previous = -1
    for line, row in enumerate(_rows(trial / f'{name}-memory.csv'), 2):
        event = row['event']
        item = {key: int(value) for key, value in row.items() if key != 'event'}
        cycle = item['cycle']
        assert cycle >= previous and item['bytes'] > 0, (line, row)
        assert item['write'] in (0, 1) and item['vector'] in (0, 1), (line, row)
        previous = cycle
        key = tuple(item[field] for field in FIELDS)
        if event == 'issue':
            pending[key].append(item)
            totals['issues'] += 1
            destinations = [totals]
            for identity, phase in phases.items():
                if _inside(cycle, phase):
                    item['phase'] = identity
                    accesses[identity].append(item)
                    destinations.append(counts[identity])
            for count in destinations:
                direction = 'write' if item['write'] else 'read'
                count[f'{direction}_bytes'] += item['bytes']
                if item['vector']:
                    count['vector_memory_beats'] += 1
                    count[f'vector_{direction}_bytes'] += item['bytes']
        elif event == 'ready':
            assert pending[key], ('Completion without issue', line, row)
            issued = pending[key].popleft()
            if not pending[key]:
                del pending[key]
            assert cycle > issued['cycle'], ('Nonpositive memory latency', issued, row)
            issued['ready_cycle'] = cycle
            totals['ready'] += 1
            if 'phase' in issued:
                identity = issued['phase']
                assert cycle <= phases[identity]['end_cycle'], ('Access outlived task', identity, issued)
                counts[identity]['summed_memory_latency_cycles'] += cycle - issued['cycle']
        else:
            raise AssertionError(('Unknown memory event', row))
    assert not pending and totals['issues'] == totals['ready'], (pending, totals)
    for key in ('read_bytes', 'write_bytes', 'vector_memory_beats', 'vector_read_bytes', 'vector_write_bytes'):
        assert totals[key] == cpu[key], ('Memory trace/stat mismatch', key, totals, cpu)
        for identity, phase in phases.items():
            assert counts[identity][key] == phase[key], ('Memory task mismatch', identity, key, counts[identity], phase)
    assert not accesses[0], ('Empty marker has data traffic', accesses[0])
    return accesses, counts


def _coverage(accesses, case, warm=False):
    count, app = case['elements'], case['application']
    output = WARM if warm else OUTPUT
    x_words = count + 2 if app == 'stencil' else count
    required = {'x': (INPUT_X, x_words, 0)}
    if app == 'add':
        required['y'] = INPUT_Y, count, 0
    if app != 'checksum':
        required['out'] = output, count, 1
    coverage = {name: [0] * words for name, (_, words, _) in required.items()}
    buffers = ((INPUT_X, (count + 2) * 4), (INPUT_Y, count * 4),
               (OUTPUT, count * 4), (WARM, count * 4))
    traffic, extra = Counter(), Counter()
    for item in accesses:
        begin, end = item['address'], item['address'] + item['bytes']
        for address, size in buffers:
            if begin < address + size + 64 and end > address - 64:
                assert address <= begin < end <= address + size, ('Access touches buffer guard', item, address, size)
        covered = False
        for name, (address, words, write) in required.items():
            if begin < address + words * 4 and end > address:
                assert address <= begin < end <= address + words * 4, ('Access exceeds application input', name, item)
                assert item['write'] == write and begin % 4 == end % 4 == 0, ('Wrong payload operation', name, item)
                for index in range((begin - address) // 4, (end - address) // 4):
                    coverage[name][index] += 1
                direction = 'write' if write else 'read'
                traffic[f'{direction}_bytes'] += item['bytes']
                # The trace flag identifies coalesced beats, not every RVV instruction.
                traffic[f'{"coalesced" if item["vector"] else "other"}_{direction}_bytes'] += item['bytes']
                covered = True
                break
        if not covered:
            assert not any(begin < address + size and end > address for address, size in buffers), (
                'Unexpected traffic to an unused application buffer', item, app, warm)
            extra['write_bytes' if item['write'] else 'read_bytes'] += item['bytes']
    for name, multiplicities in coverage.items():
        assert all(multiplicities), ('Missing application payload coverage', name,
                                    [i for i, n in enumerate(multiplicities) if not n][:10])
    return dict(actual_payload_read_bytes=traffic['read_bytes'], actual_payload_write_bytes=traffic['write_bytes'],
                payload_coalesced_vector_read_bytes=traffic['coalesced_read_bytes'],
                payload_coalesced_vector_write_bytes=traffic['coalesced_write_bytes'],
                payload_other_read_bytes=traffic['other_read_bytes'], payload_other_write_bytes=traffic['other_write_bytes'],
                extra_read_bytes=extra['read_bytes'], extra_write_bytes=extra['write_bytes'],
                coverage={name: dict(words=len(values), min_reads_or_writes=min(values),
                    max_reads_or_writes=max(values), total_word_accesses=sum(values)) for name, values in coverage.items()})


def _banks(trial, phase, case, cpu, spm, accesses):
    p = case['parameters']
    expected = Counter()
    for item in accesses:
        for address, size in _fragments(item['address'] - BASE, item['bytes'], p['spm_request_bytes']):
            expected[address, size, item['write']] += 1
    pending, seen = {}, Counter()
    counts, byte_counts, services = Counter(), Counter(), Counter()
    timed_counts, timed_bytes = Counter(), Counter()
    ports, channels, channel_requests = set(), Counter(), {}
    service_cycle = previous_cycle = -1
    active_cycles, first, last = 0, None, None
    traces = list(trial.glob('scratchpad*.csv'))
    assert len(traces) == 1, traces
    for line, row in enumerate(_rows(traces[0]), 2):
        event, identity = row['event'], row['id']
        cycle, address, size, write = (int(row[key]) for key in ('cycle', 'address', 'bytes', 'write'))
        assert cycle >= previous_cycle, (line, row)
        previous_cycle = cycle
        if event == 'accepted':
            assert identity not in pending and row['requestor'].endswith(':qemu_memory'), (line, row)
            assert write in (0, 1) and 0 <= address < p['spm_capacity_bytes'], (line, row)
            assert 0 < size <= min(p['spm_request_bytes'] - address % p['spm_request_bytes'], p['spm_capacity_bytes'] - address), (line, row)
            timed = _inside(cycle, phase)
            if timed:
                key = address, size, write
                seen[key] += 1
                assert seen[key] <= expected[key], ('Unexpected timed backend request', line, row)
                timed_counts[event] += 1
                timed_bytes[write] += size
            pending[identity] = dict(address=address, size=size, write=write, issued=0, last_service=-1, timed=timed)
            counts[event] += 1
            byte_counts[write] += size
        elif event in ('service', 'completed'):
            assert identity in pending, ('Unknown backend request', line, row)
            request = pending[identity]
            assert write == request['write'], (line, row, request)
            if event == 'service':
                if cycle != service_cycle:
                    assert cycle > service_cycle, (line, row)
                    service_cycle, ports, channels, channel_requests = cycle, set(), Counter(), {}
                bank, port, channel = (int(row[key]) for key in ('bank', 'port', 'channel'))
                assert address == request['address'] + request['issued'], (line, row, request)
                assert bank == address // p['spm_bank_width'] % p['spm_banks'], (line, row)
                assert 0 < size <= min(request['size'] - request['issued'], p['spm_bank_width'] - address % p['spm_bank_width']), (line, row)
                limit = p['spm_write_ports_per_bank'] if write else p['spm_read_ports_per_bank']
                assert 0 <= port < limit and (bank, port, write) not in ports, ('Bank port reused', line, row)
                ports.add((bank, port, write))
                assert 0 <= channel < p['spm_channels'] and channel_requests.setdefault(channel, identity) == identity, ('Channel conflict', line, row)
                channels[channel] += size
                assert channels[channel] <= p['spm_channel_width'], ('Channel byte limit', line, row)
                request['issued'] += size
                request['last_service'] = cycle
                services[write] += 1
                assert request['timed'] == _inside(cycle, phase), ('Service crosses phase boundary', line, row)
                if request['timed'] and last != cycle:
                    active_cycles += 1
                    last = cycle
                    if first is None:
                        first = cycle
            else:
                assert address == request['address'] and size == request['size'] == request['issued'], (line, row, request)
                assert cycle == request['last_service'] + 1, ('Backend completion latency', line, row, request)
                counts[event] += 1
                if request['timed']:
                    assert cycle <= phase['end_cycle'], (line, row, request)
                    timed_counts[event] += 1
                del pending[identity]
        else:
            raise AssertionError(('Unknown backend event', row))
    assert not pending and seen == expected, (pending, seen - expected, expected - seen)
    assert counts['accepted'] == counts['completed'] == cpu['memory_requests'] == spm['accepted'] == spm['completed'], (counts, cpu, spm)
    assert byte_counts[0] == spm['read_bytes'] and byte_counts[1] == spm['write_bytes'], (byte_counts, spm)
    assert services[0] == spm['read_service_beats'] and services[1] == spm['write_service_beats'], (services, spm)
    assert timed_counts['accepted'] == timed_counts['completed'] == phase['memory_requests'] == phase['completed_requests'], (timed_counts, phase)
    assert timed_bytes[0] == phase['read_bytes'] and timed_bytes[1] == phase['write_bytes'], (timed_bytes, phase)
    assert active_cycles > 0 and first is not None
    return dict(bank_service_active_cycles=active_cycles, bank_service_elapsed_cycles=last-first+1,
        bank_service_bytes_checked=sum(timed_bytes.values()), request_fragments_checked=counts['accepted'],
        timed_fragments_checked=sum(seen.values()))


def _backing(trial, case):
    count, app = case['elements'], case['application']
    path = trial / 'scratchpad.bin'
    assert path.stat().st_size == case['parameters']['spm_capacity_bytes'], path.stat()
    x, y = [input_x(i) for i in range(count + 2)], [input_y(i) for i in range(count)]
    expected = {'copy': lambda i: x[i], 'add': lambda i: (x[i] + y[i]) & MASK,
                'checksum': lambda i: 0, 'stencil': lambda i: (x[i] + x[i+1] + x[i+2]) & MASK}[app]
    with path.open('rb') as memory:
        def read_guarded(address, words):
            memory.seek(address - BASE - 64)
            before, data, after = memory.read(64), memory.read(words * 4), memory.read(64)
            assert before == after == b'\xa5' * 64, ('Buffer guard changed', hex(address), before.hex(), after.hex())
            assert len(data) == words * 4
            return struct.unpack(f'<{words}I', data)
        assert read_guarded(INPUT_X, count + 2) == tuple(x), 'Input x changed'
        assert read_guarded(INPUT_Y, count) == tuple(y), 'Input y changed'
        for address in (OUTPUT, WARM):
            for index, value in enumerate(read_guarded(address, count)):
                assert value == expected(index), ('Incorrect output', hex(address), index, value, expected(index))
        memory.seek(MAILBOX - BASE)
        actual = struct.unpack('<3I', memory.read(12))
        result = sum(x[:count]) & MASK if app == 'checksum' else 0
        assert actual == (result, result, 0xa991ca7e), ('Scalar return/mailbox mismatch', actual, result)
    return dict(input_words_checked=2*count+2, output_words_checked=2*count,
                guard_bytes_checked=512, expected_scalar_result=result)


def _lsq(trial, name, phase, cpu, case, accesses):
    depth = case["lsq_depth"]
    active, slots, order = {}, set(), deque()
    counts, stalls, timed_stalls = Counter(), Counter(), Counter()
    peak = timed_peak = 0
    previous_cycle = previous_token = -1
    timed = []
    for line, row in enumerate(_rows(trial / f"{name}-lsq.csv"), 2):
        event, reason = row["event"], row["reason"]
        item = {key: int(value) for key, value in row.items() if key not in ("event", "reason")}
        cycle, token, slot = (item[key] for key in ("cycle", "token", "slot"))
        assert cycle >= previous_cycle, (line, row)
        previous_cycle = cycle
        if event == "stall":
            assert token == slot == item["address"] == item["bytes"] == item["write"] == 0, (line, row)
            assert reason in ("register", "full", "drain"), (line, row)
            stalls[reason] += 1
            if _inside(cycle, phase):
                timed_stalls[reason] += 1
        elif event == "enqueue":
            assert not reason and token > previous_token and token not in active, (line, row)
            assert 0 <= slot < depth and slot not in slots, (line, row)
            assert 0 < item["bytes"] <= case["vlen_bits"] // 8 and item["write"] in (0, 1), (line, row)
            previous_token = token
            active[token] = item
            slots.add(slot)
            order.append(token)
            item["enqueue"] = cycle
            peak = max(peak, len(active))
            if _inside(cycle, phase):
                timed.append(item)
                timed_peak = max(timed_peak, len(active))
            counts[event] += 1
        else:
            assert not reason and token in active, ("Unknown LSQ token", line, row)
            entry = active[token]
            assert all(item[key] == entry[key] for key in ("slot", "pc", "address", "bytes", "write")), (line, row, entry)
            assert event in ("issue", "service_complete", "complete") and event not in entry, (line, row, entry)
            entry[event] = cycle
            if event == "issue":
                assert cycle == entry["enqueue"], (line, row, entry)
            elif event == "service_complete":
                assert cycle > entry["issue"], (line, row, entry)
            else:
                assert cycle >= entry["service_complete"] and order.popleft() == token, ("Retirement order", line, row, entry)
                del active[token]
                slots.remove(slot)
            counts[event] += 1
        assert item["occupancy"] == len(active) <= depth, ("LSQ occupancy", line, row, len(active))
    assert not active and not slots and not order, (active, slots, order)
    assert counts["enqueue"] == counts["issue"] == counts["service_complete"] == counts["complete"] == cpu["lsq_enqueued"] == cpu["lsq_completed"], (counts, cpu)
    assert peak == cpu["lsq_peak_occupancy"] and cpu["load_store_queue_depth"] == depth, (peak, cpu)
    for reason in ("register", "full", "drain"):
        assert stalls[reason] == cpu[f"lsq_{reason}_stalls"], (reason, stalls, cpu)
    assert len(timed) == phase["lsq_enqueued"] == phase["lsq_completed"], (len(timed), phase)
    assert 0 <= phase["lsq_stall_cycles"] <= phase["cycles"], phase
    memory = Counter((item["cycle"], item["ready_cycle"], *(item[key] for key in FIELDS)) for item in accesses)
    for entry in timed:
        key = (entry["issue"], entry["complete"], entry["pc"], entry["address"], entry["bytes"], entry["write"], 1)
        assert memory[key] > 0 and entry["complete"] <= phase["end_cycle"], ("LSQ/memory trace mismatch", entry)
        memory[key] -= 1
    if depth == 1:
        assert not counts and not stalls and cpu["lsq_stall_cycles"] == 0, (counts, stalls, cpu)
    return dict(lsq_peak_occupancy=timed_peak, lsq_peak_occupancy_total=peak,
                lsq_stall_events=dict(timed_stalls), lsq_tokens_checked=counts["enqueue"])


def validate(trial, case, log):
    trial = Path(trial)
    p, count = case['parameters'], case['elements']
    assert case['application'] in APPLICATIONS and case['implementation'] in IMPLEMENTATIONS, case
    assert case['vlen_bits'] in VLENS and p['spm_banks'] in BANKS and case['lsq_depth'] in DEPTHS, case
    assert count > 0 and count % 64 == 0, count
    assert p['riscv_vector_length_bits'] == case['vlen_bits'], case
    assert case['cpu_parameters']['load_store_queue_depth'] == case['lsq_depth'], case
    assert p['spm_capacity_bytes'] == 32*1024*1024 and p['spm_bank_width'] == 4 and p['spm_request_bytes'] == 32, p
    cpu, spm = _common._stats(log, 'RISCV_STATS'), _common._stats(log, 'SPM_STATS')
    assert not any(line.startswith('ARRAY_STATS ') for line in log.splitlines()), 'Unexpected analog component'
    assert cpu['memory_requests'] == cpu['completed_requests'], cpu
    assert cpu['read_bytes'] + cpu['fetch_bytes'] == spm['read_bytes'] and cpu['write_bytes'] == spm['write_bytes'], (cpu, spm)
    assert cpu['analog_commands'] == cpu['analog_read_bytes'] == cpu['analog_write_bytes'] == 0, cpu
    name, cache = _common._topology(trial, case)
    _common._cache(cpu, cache['instruction_cache_line_bytes'])
    phases = _common._phases(trial, name, cache['instruction_cache_line_bytes'])
    for row in _rows(trial / f'{name}-tasks.csv'):
        assert int(row['lsq_enqueued']) == int(row['lsq_completed']), ('Marker did not drain', row)
    phase, warm, empty = phases[1], phases[2], phases[0]
    # Full-kernel warmup executes every measured path with identical count/data.
    for key in ('icache_misses', 'icache_fills', 'icache_fill_bytes', 'fetch_bytes'):
        assert phase[key] == 0, ('Measured kernel is not instruction-cache warm', key, phase)
    accesses, memory = _memory(trial, name, phases, cpu)
    coverage = _coverage(accesses[1], case)
    warm_coverage = _coverage(accesses[2], case, warm=True)
    queue = _lsq(trial, name, phase, cpu, case, accesses[1])
    banks = _banks(trial, phase, case, cpu, spm, accesses[1])
    backing = _backing(trial, case)
    app = case['application']
    # Algorithmic traffic counts operand uses. Stencil reuse can reduce actual traffic.
    useful_read = count * 4 * {'copy': 1, 'add': 2, 'checksum': 1, 'stencil': 3}[app]
    useful_write = count * 4 if app != 'checksum' else 0
    compulsory_read = ((count+2)*4 if app == 'stencil' else useful_read)
    actual_read, actual_write = phase['read_bytes'], phase['write_bytes']
    assert phase['cycles'] > 0 and phase['vector_instructions'] > 0, phase
    return dict(case=case['name'], application=app, implementation=case['implementation'],
        elements=count, vlen_bits=case['vlen_bits'], spm_banks=p['spm_banks'], lsq_depth=case['lsq_depth'],
        cycles=phase['cycles'], cycles_per_element=phase['cycles']/count,
        useful_read_bytes=useful_read, useful_write_bytes=useful_write,
        compulsory_read_bytes=compulsory_read, useful_total_bytes=useful_read+useful_write,
        useful_bytes_per_cycle=(useful_read+useful_write)/phase['cycles'],
        actual_read_bytes=actual_read, actual_write_bytes=actual_write,
        actual_read_bytes_per_cycle=actual_read/phase['cycles'], actual_write_bytes_per_cycle=actual_write/phase['cycles'],
        actual_bytes_per_cycle=(actual_read+actual_write)/phase['cycles'],
        instructions=phase['instructions'], vector_instructions=phase['vector_instructions'],
        icache_misses=phase['icache_misses'], empty_marker_cycles=empty['cycles'],
        vector_memory_beats=phase['vector_memory_beats'], lsq_enqueued=phase['lsq_enqueued'],
        lsq_completed=phase['lsq_completed'], lsq_stall_cycles=phase['lsq_stall_cycles'],
        summed_memory_latency_cycles=memory[1]['summed_memory_latency_cycles'],
        phase=phase, warmup_phase=warm, empty_phase=empty, stats=dict(cpu=cpu, spm=spm),
        topology_verified=True, validation=backing, warmup_coverage=warm_coverage, **coverage, **queue, **banks)
