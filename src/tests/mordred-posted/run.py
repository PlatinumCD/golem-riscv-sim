"""Validate posted admission, finite receiver slots, commit visibility and credits."""
import argparse
from collections import defaultdict, deque
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CASES = [
    dict(name='bulk-slots1', sparse=False, slots=1, arrivals=True, credit_batch=4, credit_delay=4, link_bits=128),
    dict(name='bulk-slots2', sparse=False, slots=2, arrivals=True, credit_batch=2, credit_delay=64, link_bits=128),
    dict(name='sparse-slots2', sparse=True, slots=2, arrivals=True, credit_batch=2, credit_delay=16, link_bits=512),
    dict(name='posted-disabled', sparse=False, slots=0, arrivals=True, credit_batch=4, credit_delay=4, link_bits=128),
    dict(name='missing-arrivals', sparse=False, slots=2, arrivals=False, credit_batch=2, credit_delay=4, link_bits=128),
]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rows(path):
    with Path(path).open(newline='') as stream:
        return list(csv.DictReader(stream))


def reports(log, label):
    return [json.loads(line[len(label)+1:]) for line in log.splitlines() if line.startswith(label+' ')]


def pattern(index, size):
    return bytes((37*index+11*j+3) % 251 for j in range(size))


def validate(trial, case):
    trial = Path(trial)
    log = (trial/'simulation.log').read_text()
    fixture, = reports(log, 'MORDRED_POSTED_TEST')
    assert fixture['passed']
    count = 13 if case['slots'] and case['arrivals'] else 0
    payload = 8 if case['sparse'] else 256
    address = lambda index: 0x108+16*index if case['sparse'] else 0x100+256*index
    endpoints = sorted(reports(log, 'MORDRED_SPM_STATS'), key=lambda item: item['tile_id'])
    assert len(endpoints) == 2
    source, target = endpoints
    assert fixture['accepted'] == fixture['committed'] == count
    assert fixture['checked_bytes'] == count*payload+8
    assert fixture['busy'] == int(bool(count))
    assert fixture['released_before_last_commit'] == bool(count)
    expected = bytearray(b'\x11'*4096)
    expected[0xf08:0xf10] = pattern(31, 8)
    for index in range(count):
        expected[address(index):address(index)+payload] = pattern(index, payload)
    assert (trial/'tile0-spm.bin').read_bytes() == b'\x11'*4096
    assert (trial/'tile1-spm.bin').read_bytes() == expected
    assert source['requests_sent'] == target['requests_received'] == count+2
    assert target['responses_sent'] == source['responses_received'] == 2
    assert source['responses_sent'] == target['requests_sent'] == 0
    assert source['posted_accepted'] == target['posted_committed'] == count
    assert source['posted_committed'] == target['posted_accepted'] == 0
    assert target['arrivals_sent'] == (count+1 if case['arrivals'] else 0)
    assert source['arrivals_sent'] == 0
    assert source['credits_received'] == target['credits_sent'] == count
    assert source['credit_packets_received'] == target['credit_packets_sent']
    assert source['credit_packets_sent'] == target['credit_packets_received'] == 0
    assert source['bytes_read'] == source['bytes_written'] == 0
    assert target['bytes_read'] == target['bytes_written'] == count*payload+8
    for endpoint in endpoints:
        assert endpoint['idle'] is True
        assert endpoint['max_pending_memory'] <= 1
        assert endpoint['max_local_requests'] <= 4
        assert endpoint['max_posted_receive_reservations'] <= case['slots']
        assert endpoint['max_pending_credit_slots'] <= case['slots']
        assert endpoint['max_reserved_receiver_slots'] <= case['slots']
        assert endpoint['packets_injected'] == endpoint['requests_sent']+endpoint['responses_sent']+endpoint['credit_packets_sent']
        assert endpoint['remote_rejected'] == endpoint['local_bank_rejected'] == 0
    flit = case['link_bits']//8
    wire = lambda header, data=0: max(2, (header+data+flit-1)//flit)*flit
    assert source['wire_bytes_sent'] == count*wire(40, payload)+wire(40, 8)+wire(40)
    assert target['credit_wire_bytes_sent'] == target['credit_packets_sent']*wire(16)
    assert target['wire_bytes_sent'] == wire(40)+wire(40, 8)+target['credit_wire_bytes_sent']
    assert source['wire_bytes_received'] == target['wire_bytes_sent']
    assert target['wire_bytes_received'] == source['wire_bytes_sent']
    assert source['credit_wire_bytes_received'] == target['credit_wire_bytes_sent']
    if count:
        assert source['posted_credit_stall_cycles'] > 0
        assert source['max_local_requests'] == 4
        assert source['max_reserved_receiver_slots'] == case['slots']
        assert target['max_posted_receive_reservations'] == case['slots']
        if case['slots'] > 1:
            assert target['credit_packets_sent'] < count, 'Expected aggregated credit returns'
    histories, all_events, fragments = defaultdict(lambda: defaultdict(list)), [], []
    for tile in range(2):
        for raw in rows(trial/f'posted.tile{tile}.router_spm-spm.csv'):
            row = {key: value if key == 'event' else int(value) for key, value in raw.items()}
            all_events.append(row)
            if row['event'].startswith('credit_'):
                continue
            histories[row['source'], row['request_id']][row['event']].append(row)
    driver = [{key: value if key in ('event', 'label', 'data_hex') else int(value)
               for key, value in raw.items()} for raw in rows(trial/'fixture.csv')]
    admitted, arrivals = {}, {}
    for row in driver:
        if row['event'] == 'response' and row['status'] == 5:
            assert row['id'] not in admitted and row['posted'] and not row['arrival'] and not row['data_hex']
            admitted[row['id']] = row
        if row['event'] == 'arrival' and row['posted']:
            assert row['id'] not in arrivals and row['status'] == 0 and row['arrival'] and not row['data_hex']
            arrivals[row['id']] = row
        if row['event'] == 'request' and row['label'] == 'committed_read':
            index = row['id']-1000
            assert row['cycle'] > arrivals[100+index]['cycle']
    assert set(admitted) == set(arrivals) == set(range(100, 100+count))
    for identity in admitted:
        own = histories[0, identity]
        assert all(len(own[key]) == 1 for key in ('posted_queued', 'posted_accepted', 'request_send',
            'request_recv', 'posted_commit', 'arrival'))
        assert not own['response_send'] and not own['response_recv'] and not own['response_ready']
        accepted, committed = own['posted_accepted'][0]['cycle'], own['posted_commit'][0]['cycle']
        assert own['posted_queued'][0]['cycle'] <= accepted == own['request_send'][0]['cycle']
        assert accepted < admitted[identity]['cycle'] < committed < arrivals[identity]['cycle']
        assert own['arrival'][0]['cycle'] == committed
        assert sum(row['bytes'] for row in own['write_request']) == payload
        assert sum(row['bytes'] for row in own['write_response']) == payload
        assert max(row['cycle'] for row in own['write_response']) == committed
    # Each accepted payload consumes a destination credit before it enters
    # the network; only real received credit packets make it reusable.
    reserved, maximum, pending, received, committed = 0, 0, 0, 0, 0
    for row in sorted(all_events, key=lambda item: (item['cycle'], item['tile'])):
        event = row['event']
        if event == 'posted_accepted' and row['tile'] == 0:
            reserved += 1; maximum = max(maximum, reserved)
            assert reserved <= case['slots']
        elif event == 'credit_recv' and row['tile'] == 0:
            reserved -= row['bytes']; received += row['bytes']; assert reserved >= 0
        elif event == 'posted_commit' and row['tile'] == 1:
            pending += 1; committed += 1
        elif event == 'credit_send' and row['tile'] == 1:
            assert 0 < row['bytes'] <= min(case['credit_batch'], case['slots'])
            pending -= row['bytes']; assert pending >= 0
    assert reserved == pending == 0 and received == committed == count
    assert maximum == source['max_reserved_receiver_slots']
    # Join every endpoint fragment with actual bank acceptance/service/finish.
    for tile in range(2):
        ep = [row for row in all_events if row['tile'] == tile]
        memory = {}
        for row in ep:
            if row['event'] in ('read_request', 'write_request'):
                assert row['memory_request_id'] not in memory
                memory[row['memory_request_id']] = dict(row)
            elif row['event'] in ('read_response', 'write_response'):
                item = memory.pop(row['memory_request_id'])
                assert all(item[key] == row[key] for key in ('address', 'bytes', 'write', 'request_id'))
                fragments.append(dict(tile=tile, start=item['cycle'], end=row['cycle'],
                    address=row['address'], bytes=row['bytes'], write=row['write']))
        assert not memory
        bank_files = list(trial.glob(f'posted.tile{tile}.scratchpad*.csv'))
        assert len(bank_files) == 1
        bank, by_key = {}, defaultdict(deque)
        for raw in rows(bank_files[0]):
            event, identity = raw['event'], raw['id']
            row = {key: int(raw[key]) for key in ('cycle', 'address', 'bytes', 'write', 'bank')}
            if event == 'accepted':
                assert identity not in bank
                bank[identity] = dict(row, served=0, last=row['cycle'])
            elif event == 'service':
                item = bank[identity]
                expected_bank = row['address']//4 % (4 if case['sparse'] else 1)
                assert row['bank'] == expected_bank and (not case['sparse'] or expected_bank in (2, 3))
                assert row['address'] == item['address']+item['served']
                assert 0 < row['bytes'] <= 4-row['address'] % 4
                item['served'] += row['bytes']; item['last'] = row['cycle']
            else:
                assert event == 'completed'
                item = bank.pop(identity)
                assert item['served'] == item['bytes'] and item['last'] < row['cycle']
                by_key[item['address'], item['bytes'], item['write']].append((item['cycle'], row['cycle']))
        assert not bank
        for item in sorted((f for f in fragments if f['tile'] == tile), key=lambda f: f['start']):
            begin, end = by_key[item['address'], item['bytes'], item['write']].popleft()
            assert item['start'] < begin < end < item['end']
        assert not any(by_key.values())
    stats = [{key.strip(): value.strip() for key, value in raw.items()}
             for raw in rows(trial/'network-statistics.csv')]
    for tile, port in ((0, 1), (1, 3)):
        matching = [row for row in stats if row['ComponentName'] == f'fabric.router.{tile}.0'
                    and row['StatisticName'] == 'sent_flit_cnt'
                    and row['StatisticSubId'].startswith(f'{tile}_{port}_')]
        assert matching and sum(int(row['Sum.u64']) for row in matching)*flit == endpoints[tile]['wire_bytes_sent']
    return dict(passed=True, fixture=fixture, endpoints=endpoints, exact_backing_bytes=8192,
                bounded_credit_conservation=True, arrival_after_bank_commit=True,
                no_source_commit_ack=True, rejected_writes_do_not_mutate_memory=True,
                physical_bank_fragments=len(fragments), actual_credit_wire_bytes=target['credit_wire_bytes_sent'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-info', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'tests/results/mordred-posted'/str(time.time_ns()))
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    info = json.loads(args.build_info.read_text())
    build = args.build_info.resolve().parent
    assert str(HERE/'fixturedriver.cc') in info['command'], 'Build must include mordred-posted/fixturedriver.cc'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.check_only)
    paths = [build/'libtilecomponents.so', build/'libmordred.so', args.build_info,
             HERE/'fixturedriver.cc', HERE/'simulation.py', HERE/'run.py']
    hashes = {str(path.resolve()): sha(path) for path in paths}
    validated = []
    for case in CASES:
        trial = output/case['name']
        if not args.check_only:
            trial.mkdir()
            (trial/'case.json').write_text(json.dumps(case, indent=2)+'\n')
            command = [info['sst'], '--num-threads=1', f'--add-lib-path={build}',
                       f'--output-json={trial/"topology.json"}', str(HERE/'simulation.py')]
            env = os.environ | dict(TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1')
            env.pop('TILE_COMPONENT_TRACE_START_TASK', None)
            env.pop('TILE_COMPONENT_PROGRAM_PROOF', None)
            with (trial/'simulation.log').open('w') as log:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
            assert result.returncode == 0, (case['name'], (trial/'simulation.log').read_text()[-6000:])
        result = validate(trial, case)
        (trial/'validation.json').write_text(json.dumps(result, indent=2)+'\n')
        validated.append(dict(case=case['name'], **result))
        print('PASS', case['name'], flush=True)
    assert hashes == {name: sha(name) for name in hashes}, 'Inputs changed during run'
    (output/'validation.json').write_text(json.dumps(dict(passed=True, cases=validated, sha256=hashes), indent=2)+'\n')
    print('PASS', output/'validation.json')


if __name__ == '__main__':
    main()
