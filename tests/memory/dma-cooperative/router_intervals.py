"""Audit one-word router service and preserve queue/service/idle intervals."""
import argparse
import collections
import csv
import json
from pathlib import Path


def complement(intervals, low, high):
    idle = []
    cursor = low
    for start, finish in sorted(intervals):
        start, finish = max(low, start), min(high, finish)
        if finish <= start:
            continue
        assert start >= cursor, 'overlapping service on one physical output'
        if start > cursor:
            idle.append([cursor, start])
        cursor = finish
    if cursor < high:
        idle.append([cursor, high])
    return idle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    results = json.loads((args.run/'results.json').read_text())
    assert json.loads((args.run/'complete.json').read_text()) == {'status':'PASS', 'cases':len(results)}
    total = 0
    for result in results:
        trial = Path(result['directory'])
        timeline = json.loads((trial/'timeline.json').read_text())
        low, high = (t/1000 for t in timeline['cpu_region'])
        low = min([low]+[int(r['injection_tick'])/1000 for r in timeline['requests']])
        high = max([high]+[int(r['event_tick'])/1000 for r in timeline['peer_received']])
        outputs = {}
        count = 0
        for router in range(2):
            pending = {}
            serviced = collections.defaultdict(list)
            queued = collections.defaultdict(list)
            with (trial/'profile'/f'router-{router}-packets.csv').open() as stream:
                for row in csv.DictReader(stream):
                    key = (row['source'], row['packet_id'], row['output'])
                    cycle = int(row['cycle'])
                    if row['event'] == 'grant':
                        assert key not in pending
                        ready = int(row['head_ready_cycle'])
                        assert ready <= cycle
                        pending[key] = cycle
                        queued[row['output']].append([ready, cycle])
                    else:
                        assert row['event'] == 'tail'
                        start = pending.pop(key)
                        assert start == cycle, 'this diagnostic sends exactly one flit per packet'
                        serviced[row['output']].append([start, cycle+1])
                        count += 1
            assert not pending
            for output, intervals in serviced.items():
                outputs[f'router-{router}/output-{output}'] = {
                    'service_intervals': intervals,
                    'queue_intervals': queued[output],
                    'idle_intervals': complement(intervals, low, high),
                    'service_cycles': sum(b-a for a,b in intervals),
                    'queue_cycles': sum(b-a for a,b in queued[output]),
                }
        assert count == result['rounds']*4  # request/reply, two routers each
        total += count
        dma = timeline['dma']
        start, finish = int(dma['service_start_cycle']), int(dma['completion_cycle'])
        report = {
            'units':'1 GHz cycles; half-open intervals', 'window':[low,high],
            'network_outputs':outputs,
            'ram':{'queue_intervals':[[int(dma['arrival_cycle']), start]],
                   'service_intervals':[[start,finish]],
                   'idle_intervals':complement([[start,finish]], low,high)},
            'note':'Router queue time starts after pipeline readiness. Packet transit is not link busy time.',
        }
        (trial/'resource-intervals.json').write_text(json.dumps(report,indent=2)+'\n')
    summary = {'status':'PASS','cases':len(results),'verified_router_services':total}
    (args.run/'router-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
