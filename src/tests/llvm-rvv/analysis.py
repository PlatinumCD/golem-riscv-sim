"""Common, independently reconciled measurements for CPU/SPM experiments.

Instruction-cache lookups identify executed PCs in successful, single-issue,
fault-free runs. Their timed counts MUST equal the retired task counters. Memory
beats are attributed to those instructions, never counted as instructions.
"""
from collections import Counter, defaultdict, deque
from bisect import bisect_right
import csv
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SCHEMA = 1


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rows(path):
    with Path(path).open(newline='') as stream:
        yield from csv.DictReader(stream)


def one(trial, suffix):
    paths = list(Path(trial).glob('*' + suffix))
    if suffix == '-waits.csv':
        # Analog command-queue waits are a separate trace, not CPU/LSQ waits.
        paths = [path for path in paths if not path.name.endswith('-asq-waits.csv')]
    assert len(paths) == 1, (suffix, paths)
    return paths[0]


def phase_of(trial, task_id):
    pair = [r for r in rows(one(trial, '-tasks.csv')) if int(r['task_id']) == task_id]
    assert len(pair) == 2 and [r['event'] for r in pair] == ['start', 'finish'], pair
    start, finish = [{k: int(v) for k, v in r.items() if k != 'event'} for r in pair]
    assert start['cycle'] < finish['cycle'] and start['execution_id'] == finish['execution_id']
    for r in (start, finish):
        assert r['memory_requests'] == r['completed_requests']
        assert r['lsq_enqueued'] == r['lsq_completed']
    delta = {k: finish[k] - start[k] for k in start if k not in ('task_id', 'execution_id')}
    return dict(start_cycle=start['cycle'], end_cycle=finish['cycle'],
                **{('cycles' if k == 'cycle' else k): v for k, v in delta.items()})


def classify(raw, mnemonic):
    opcode = raw & 127
    if opcode in (7, 39) and ((raw >> 12) & 7) in (0, 5, 6, 7):
        return 'vector_load' if opcode == 7 else 'vector_store'
    if opcode == 87:
        return 'vector_config' if (raw >> 12) & 7 == 7 else 'vector_alu'
    if mnemonic in ('lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld', 'flw', 'fld'):
        return 'scalar_load'
    if mnemonic in ('sb', 'sh', 'sw', 'sd', 'fsw', 'fsd'):
        return 'scalar_store'
    if opcode in (99, 103, 111) or mnemonic in ('beqz', 'bnez', 'j', 'jr', 'jal', 'jalr', 'ret', 'call', 'tail'):
        return 'scalar_control'
    return 'scalar_other'


@lru_cache(maxsize=256)
def elf_information(elf):
    """Read exact function extents, and decode using the installed target tools."""
    elf = Path(elf)
    blob = elf.read_bytes()
    assert blob[:6] == b'\x7fELF\x02\x01', 'Expected ELF64 little endian'
    shoff, = struct.unpack_from('<Q', blob, 40)
    shentsize, shnum = struct.unpack_from('<HH', blob, 58)
    assert shentsize == 64 and shnum
    sections = [struct.unpack_from('<IIQQQQIIQQ', blob, shoff + i * shentsize) for i in range(shnum)]
    functions = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        string_data = blob[strings[4]:strings[4] + strings[5]]
        assert section[9] == 24
        for offset in range(section[4], section[4] + section[5], 24):
            name, info, _, index, address, size = struct.unpack_from('<IBBHQQ', blob, offset)
            if index and info & 15 == 2 and size:
                label = string_data[name:string_data.index(0, name)].decode()
                functions[label] = (address, address + size)
    disassembly = subprocess.check_output([str(ROOT / 'install/llvm/bin/llvm-objdump'), '-d', str(elf)], text=True)
    instructions = {}
    for line in disassembly.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)(.*)$', line)
        if match:
            pc, raw, mnemonic, operands = match.groups()
            instructions[int(pc, 16)] = dict(raw=int(raw, 16), mnemonic=mnemonic,
                operands=operands.strip(), category=classify(int(raw, 16), mnemonic))
    assert functions and instructions
    return instructions, functions, hashlib.sha256(blob).hexdigest()


def count_summary(counts):
    result = dict(sorted(counts.items()))
    total = sum(counts.values())
    vectors = sum(v for k, v in counts.items() if k.startswith('vector_'))
    memory = counts['vector_load'] + counts['vector_store']
    return dict(classes=result, total=total, vector_instructions=vectors,
                vector_memory_instructions=memory,
                vector_density=vectors / total if total else 0,
                vector_memory_density=memory / total if total else 0)


def analyze_trial(trial, elf, kernel_symbols=('transfer_kernel',), task_id=1):
    trial, elf = Path(trial), Path(elf)
    case = json.loads((trial / 'case.json').read_text())
    parameters = case['parameters']
    phase = phase_of(trial, task_id)
    start, end, cycles = (phase[k] for k in ('start_cycle', 'end_cycle', 'cycles'))
    instructions, functions, elf_sha = elf_information(str(elf.resolve()))
    extents = [functions[name] for name in kernel_symbols]
    assert extents, kernel_symbols
    dynamic, kernel, histogram, pcs = Counter(), Counter(), Counter(), Counter()
    fetch_cycles = defaultdict(list)
    first_kernel, last_kernel = None, None
    lookup_count = 0
    for row in rows(one(trial, '-icache.csv')):
        if row['event'] not in ('hit', 'miss') or not start <= int(row['cycle']) < end:
            continue
        pc = int(row['address'])
        instruction = instructions[pc]
        category = instruction['category']
        dynamic[category] += 1
        histogram[instruction['mnemonic']] += 1
        pcs[pc] += 1
        fetch_cycles[pc].append(int(row['cycle']))
        lookup_count += 1
        if any(lo <= pc < hi for lo, hi in extents):
            kernel[category] += 1
            first_kernel = int(row['cycle']) if first_kernel is None else first_kernel
            last_kernel = int(row['cycle'])
    counts, kernel_counts = count_summary(dynamic), count_summary(kernel)
    assert lookup_count == phase['icache_fetches'] == phase['instructions'], (lookup_count, phase)
    assert counts['vector_instructions'] == phase['vector_instructions'], (counts, phase)
    assert phase['issue_cycles'] == phase['instructions'], 'These studies require single issue'
    assert kernel_counts['total'] > 0

    asynchronous = Counter()
    enqueue_count, completion_count = 0, 0
    occupancy, last, peak, area = 0, start, 0, 0
    occupancy_cycles = Counter()
    async_bytes = Counter()
    async_instructions = defaultdict(list)
    for row in rows(one(trial, '-lsq.csv')):
        event, cycle = row['event'], int(row['cycle'])
        if not start <= cycle <= end:
            continue
        if event == 'issue' and cycle < end:
            key = tuple(int(row[k]) for k in ('cycle', 'pc', 'address', 'bytes', 'write'))
            asynchronous[key] += 1
            async_bytes['write' if int(row['write']) else 'read'] += int(row['bytes'])
            _, pc, address, size, write = key
            assert instructions[pc]['category'] == ('vector_store' if write else 'vector_load'), row
            fetch_index = bisect_right(fetch_cycles[pc], cycle) - 1
            assert fetch_index >= 0, ('Async transfer has no executed instruction', row)
            # A register group can submit several beats before its next fetch.
            # Associate by this PC's dynamic fetch instance, not issue cycle:
            # queue-full waits can separate beats of the same instruction.
            async_instructions[pc, fetch_index].append((address, size, write))
        if event not in ('enqueue', 'complete'):
            continue
        area += occupancy * (cycle - last)
        occupancy_cycles[occupancy] += cycle - last
        last = cycle
        occupancy += 1 if event == 'enqueue' else -1
        assert occupancy == int(row['occupancy']) and occupancy >= 0, row
        peak = max(peak, occupancy)
        enqueue_count += event == 'enqueue'
        completion_count += event == 'complete'
    occupancy_cycles[occupancy] += end - last
    area += occupancy * (end - last)
    assert occupancy == 0 and enqueue_count == completion_count == phase['lsq_enqueued'] == phase['lsq_completed']
    assert sum(occupancy_cycles.values()) == cycles
    depth = int(case.get('lsq_depth', case.get('cpu_parameters', {}).get('load_store_queue_depth', 1)))
    assert peak <= depth
    for identity, beats in async_instructions.items():
        for left, right in zip(beats, beats[1:]):
            assert left[0] + left[1] == right[0] and left[2] == right[2], (
                'Noncontiguous beats attributed to one vector instruction', identity, beats)
    assert len(async_instructions) <= counts['vector_memory_instructions']

    waits, wait_events, wait_intervals = Counter(), Counter(), []
    for row in rows(one(trial, '-waits.csv')):
        lo, hi = int(row['start_cycle']), int(row['end_cycle'])
        if lo == hi:
            continue
        if hi <= start or lo >= end:
            continue
        assert start <= lo < hi <= end, (row, phase)
        if row['reason'] == 'drain':
            # The marker wrapper first writes its MMIO metadata, which may
            # drain before the TASK_FINISH event itself. Drains after the last
            # kernel instruction belong to final delivery, not kernel work.
            final = int(row['stop_reason']) == 8 or lo > last_kernel
            category = 'final_drain' if final else 'in_kernel_drain'
        else:
            assert row['reason'] in ('register', 'full'), row
            category = 'lsq_' + row['reason']
        waits[category] += hi - lo
        wait_events[category] += 1
        wait_intervals.append((lo, hi, category))
    assert sum(waits.values()) == phase['lsq_stall_cycles'], (waits, phase)

    pending, traffic = defaultdict(deque), Counter()
    blocking_intervals = []
    remaining_async = asynchronous.copy()
    for row in rows(one(trial, '-memory.csv')):
        cycle = int(row['cycle'])
        if cycle < start:
            continue
        if cycle > end:
            break
        key = tuple(int(row[k]) for k in ('pc', 'address', 'bytes', 'write', 'vector'))
        pc, _, size, write, coalesced = key
        if row['event'] == 'issue' and cycle < end:
            candidate = (cycle, *key[:4])
            is_async = remaining_async[candidate] > 0
            if is_async:
                remaining_async[candidate] -= 1
            pending[key].append((cycle, is_async))
            direction = 'write' if write else 'read'
            traffic[direction + '_bytes'] += size
            vector = instructions[pc]['category'] in ('vector_load', 'vector_store')
            traffic[('vector_' if vector else 'scalar_') + direction + '_bytes'] += size
            if coalesced:
                traffic['coalesced_vector_' + direction + '_bytes'] += size
        elif row['event'] == 'ready' and pending[key]:
            lo, is_async = pending[key].popleft()
            assert start <= lo < cycle <= end, row
            if not is_async:
                blocking_intervals.append((lo, cycle, 'blocking_memory'))
        else:
            assert cycle == end and row['event'] == 'issue', row
    assert not any(pending.values()) and not any(remaining_async.values())
    for direction in ('read', 'write'):
        assert traffic[direction + '_bytes'] == phase[direction + '_bytes'], (traffic, phase)
        assert traffic['coalesced_vector_' + direction + '_bytes'] == phase['vector_' + direction + '_bytes']
    assert sum(async_bytes.values()) <= traffic['vector_read_bytes'] + traffic['vector_write_bytes']
    blocks = sum(hi - lo for lo, hi, _ in blocking_intervals)
    intervals = sorted(wait_intervals + blocking_intervals)
    for a, b in zip(intervals, intervals[1:]):
        assert a[1] <= b[0], ('Overlapping CPU waits', a, b)
    attribution = dict(issue=phase['issue_cycles'], instruction_cache=phase['icache_stall_cycles'],
        blocking_memory=blocks, lsq_register=waits['lsq_register'], lsq_full=waits['lsq_full'],
        in_kernel_drain=waits['in_kernel_drain'], final_drain=waits['final_drain'])
    assert sum(attribution.values()) == cycles, ('Incomplete or overlapping cycle attribution', attribution, phase)

    # Backend utilization is concurrent with CPU issue/waits, not an elapsed-time term.
    services, service_bytes, active_cycles = Counter(), Counter(), set()
    port_slots, channel_bytes, channel_requests = set(), Counter(), {}
    last_service = -1
    accepted_bytes, completed_bytes = Counter(), Counter()
    backend_paths = list(trial.glob('scratchpad*.csv'))
    assert len(backend_paths) == 1, backend_paths
    for row in rows(backend_paths[0]):
        cycle = int(row['cycle'])
        if not start <= cycle < end:
            continue
        event, size, write = row['event'], int(row['bytes']), int(row['write'])
        direction = 'write' if write else 'read'
        if event == 'accepted':
            accepted_bytes[direction] += size
        elif event == 'completed':
            completed_bytes[direction] += size
        elif event == 'service':
            if cycle != last_service:
                assert cycle > last_service
                port_slots, channel_bytes, channel_requests = set(), Counter(), {}
                last_service = cycle
            bank, port, channel, address = (int(row[k]) for k in ('bank', 'port', 'channel', 'address'))
            limit = parameters['spm_write_ports_per_bank' if write else 'spm_read_ports_per_bank']
            assert bank == address // parameters['spm_bank_width'] % parameters['spm_banks']
            assert 0 <= port < limit and (bank, port, write) not in port_slots
            assert 0 <= channel < parameters['spm_channels']
            assert channel_requests.setdefault(channel, row['id']) == row['id']
            port_slots.add((bank, port, write))
            channel_bytes[channel] += size
            assert channel_bytes[channel] <= parameters['spm_channel_width']
            assert 0 < size <= parameters['spm_bank_width'] - address % parameters['spm_bank_width']
            active_cycles.add(cycle)
            services[direction] += 1
            service_bytes[direction] += size
        else:
            raise AssertionError(row)
    assert accepted_bytes == completed_bytes == service_bytes, (accepted_bytes, completed_bytes, service_bytes)
    assert service_bytes['read'] == traffic['read_bytes'] + phase['fetch_bytes']
    assert service_bytes['write'] == traffic['write_bytes']
    bank_width, banks = parameters['spm_bank_width'], parameters['spm_banks']
    channel_capacity = parameters['spm_channels'] * parameters['spm_channel_width']
    backend = dict(service_bytes=dict(service_bytes), service_beats=dict(services),
        active_cycles=len(active_cycles), no_service_cycles=cycles - len(active_cycles),
        channel_utilization=sum(service_bytes.values()) / (cycles * channel_capacity))
    for direction in ('read', 'write'):
        capacity = banks * bank_width * parameters[f'spm_{direction}_ports_per_bank']
        backend[direction + '_bank_byte_utilization'] = service_bytes[direction] / (cycles * capacity)
        backend[direction + '_bank_port_utilization'] = services[direction] / (cycles * capacity / bank_width)
    data_bytes = traffic['read_bytes'] + traffic['write_bytes']
    vector_bytes = traffic['vector_read_bytes'] + traffic['vector_write_bytes']
    traffic.update(dict(fetch_bytes=phase['fetch_bytes'], async_read_bytes=async_bytes['read'],
                        async_write_bytes=async_bytes['write']))
    for direction in ('read', 'write'):
        for prefix in ('', 'scalar_', 'vector_', 'coalesced_vector_'):
            traffic.setdefault(prefix + direction + '_bytes', 0)
    result = dict(schema=SCHEMA, passed=True, phase=phase, instructions=counts,
        kernel=dict(kernel_counts, symbols=list(kernel_symbols), first_fetch_cycle=first_kernel,
                    last_fetch_cycle=last_kernel),
        instruction_histogram=dict(sorted(histogram.items())),
        executed_pc_counts={hex(pc): n for pc, n in sorted(pcs.items())},
        cpu_cycles=attribution, traffic=dict(traffic),
        rates=dict(read_bytes_per_cycle=traffic['read_bytes'] / cycles,
            write_bytes_per_cycle=traffic['write_bytes'] / cycles,
            data_bytes_per_cycle=data_bytes / cycles,
            data_bytes_per_instruction=data_bytes / counts['total']),
        lsq=dict(depth=depth, enqueued=enqueue_count, mean_occupancy=area / cycles,
            peak_occupancy=peak, occupancy_cycles=dict(sorted(occupancy_cycles.items())),
            wait_cycles=dict(waits), wait_events=dict(wait_events),
            async_vector_instructions=len(async_instructions),
            split_vector_instructions=sum(len(beats) > 1 for beats in async_instructions.values()),
            max_beats_per_instruction=max(map(len, async_instructions.values()), default=0),
            async_vector_byte_fraction=sum(async_bytes.values()) / vector_bytes if vector_bytes else 0),
        backend=backend,
        provenance=dict(elf=str(elf.resolve()), elf_sha256=elf_sha,
            analysis_sha256=digest(__file__), instruction_method='executed fetch PCs reconciled with retired task counters'))
    return result
