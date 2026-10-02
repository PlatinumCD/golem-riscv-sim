"""Independent values, queue lifetimes, capacity, and precise-fault checks."""
from collections import deque
import csv
import json
import re
import struct

def records(path):
    with path.open() as f: return list(csv.DictReader(f))
def reports(log, label):
    return [json.loads(x[len(label)+1:]) for x in log.splitlines() if x.startswith(label+' ')][0]

def check(trial, depth, assembly):
    memory = (trial/'scratchpad.bin').read_bytes()
    expect = {0: -104, 8: 0x98, 16: -17768, 24:0xba98, 32:-19088744,
        40:0xfedcba98, 48:0x88776655fedcba98, 56:0x1122334455667788,
        64:23, 72:0x1122334455667788, 80:41, 88:99, 96:41,
        104:0x11223344aabb7788, 112:0x11223344aabb7788,
        120:0x0000000700000007, 128:0x0000000900000008, 136:0x0000000700000007,
        144:0x0000000900000008, 152:0x0000000200000002,160:0x0000000200000002,
        168:0xffffffff3fc00000,176:0x4014000000000000,184:0x40400000,
        192:0x4004000000000000,200:81,208:81,216:0x4004000000000000,
        224:0x55fedcba,232:0x3344556677888877,240:111,248:0x1122334455667788,
        256:5,264:0x90200000,272:4,280:0,288:0x98,296:0xffffffff3fc00000}
    for offset, expected in expect.items():
        got = struct.unpack_from('<Q', memory, 0x110000+offset)[0]
        assert got == expected & ((1<<64)-1), (trial.name,offset,hex(got),hex(expected))
    cpu = reports((trial/'simulation.log').read_text(), 'RISCV_STATS')
    assert cpu['memory_requests'] == cpu['completed_requests']
    if depth:
        events = records(trial/'riscv-slq.csv')
        active, retired, peak = {}, deque(), 0
        for row in events:
            token, cycle = int(row['token']), int(row['cycle'])
            if row['event'] == 'stall': continue
            if row['event'] == 'enqueue':
                assert token not in active
                active[token] = dict(row, enqueue=cycle)
                retired.append(token)
                peak = max(peak,len(active))
                assert len(active) <= depth
            else:
                entry = active[token]
                if row['event'] == 'service_complete':
                    assert cycle > entry['enqueue']
                    entry['serviced'] = cycle
                elif row['event'] == 'complete':
                    assert retired.popleft() == token and cycle >= entry['serviced']
                    del active[token]
        assert not active and not retired
        assert cpu['slq_enqueued'] == cpu['slq_completed'] > 2000
        assert cpu['slq_peak_occupancy'] == peak <= depth
        assert cpu['slq_register_stalls'] > 0
        if depth >= 4: assert peak >= 4
        # Handler instructions must not conceal a missing precise-fault drain.
        symbols={name:int(pc,16) for pc,name in re.findall(
            r'^([0-9a-f]+) <([^>]+)>:',assembly,re.M)}
        fetches=records(trial/'riscv-icache.csv')
        fault=next(int(r['cycle']) for r in fetches if r['event'] in ('hit','miss')
            and int(r['address'])==symbols['fault_point'])
        handler=next(int(r['cycle']) for r in fetches if r['event'] in ('hit','miss')
            and int(r['address'])==symbols['trap_handler'])
        older={r['token'] for r in events if r['event']=='enqueue' and int(r['cycle'])<fault}
        assert all(int(r['cycle'])<=handler for r in events if r['event']=='complete' and r['token'] in older)
    (trial/'verified.json').write_text(json.dumps(dict(passed=True, cpu=cpu),indent=2)+'\n')
    return cpu
