"""A deliberately small resource-load model; holdouts are never used to fit."""
from collections import Counter
import math
import statistics


def least_squares(rows, targets):
    n = len(rows[0])
    a = [[sum(r[i]*r[j] for r in rows) for j in range(n)] +
         [sum(r[i]*y for r,y in zip(rows,targets))] for i in range(n)]
    for col in range(n):
        pivot = max(range(col,n),key=lambda k:abs(a[k][col]))
        a[col],a[pivot] = a[pivot],a[col]
        if abs(a[col][col]) < 1e-12: raise ValueError('insufficient independent calibration data')
        factor = a[col][col]; a[col] = [x/factor for x in a[col]]
        for i in range(n):
            if i == col: continue
            factor = a[i][col]; a[i] = [x-factor*y for x,y in zip(a[i],a[col])]
    return [r[-1] for r in a]


def features(case):
    # Only declared workload/configuration, not measured holdout timestamps.
    source_bytes, sink_bytes, link_bytes = Counter(),Counter(),Counter()
    source_frames, sink_frames, link_frames = Counter(),Counter(),Counter()
    directions = {}
    descriptors = case['segments']*case['waves']
    for (src,dst),path in zip(case['flows'],case['paths']):
        count = case['payload']*case['waves']
        source_bytes[src] += count; sink_bytes[dst] += count
        source_frames[src] += descriptors; sink_frames[dst] += descriptors
        directions.setdefault(src,set()).add(tuple(path[0]))
        for edge in path:
            edge = tuple(edge); link_bytes[edge] += count; link_frames[edge] += descriptors
    candidates = []
    for src,count in source_bytes.items():
        lanes = min(case['tx_lanes'],len(directions[src]))
        candidates.append((f'TX tile {src}',count/lanes,source_frames[src]/lanes))
    candidates += [(f'RX tile {dst}',count,sink_frames[dst]) for dst,count in sink_bytes.items()]
    candidates += [(f'link {a}->{b}',count,link_frames[(a,b)]) for (a,b),count in link_bytes.items()]
    hops = max((len(p) for p in case['paths']),default=0)
    return candidates,hops


def validate_prediction(records):
    train = [r for r in records if r['case']['fit_role']=='train']
    holdout = [r for r in records if r['case']['fit_role']=='holdout']
    if len(train)<3 or not holdout:
        return dict(status='NOT_EVALUATED',reason='need single-flow calibration and withheld cases')
    if any(len(r['case']['flows'])!=1 for r in train): raise ValueError('calibration must be single flow')
    x = [[1,r['case']['payload']/1024,max(map(len,r['case']['paths']))] for r in train]
    coefficients = least_squares(x,[r['makespan_cycles'] for r in train])
    setup, cycles_per_kib, hop = coefficients
    if any(c<0 for c in coefficients):
        return dict(status='REJECTED',reason='negative fitted physical cost; no clipping or holdout retuning',coefficients=coefficients)
    rows = []
    for record in holdout:
        case = record['case']; resources,hops = features(case)
        scored = [(label,setup*frames+cycles_per_kib*size/1024) for label,size,frames in resources]
        label, cost = max(scored,key=lambda x:x[1])
        # Guest loop uses decrement+branch. This is a declared lower-order
        # instruction cost; polls/protocol/queue interactions remain prediction error.
        delay = 2*case['delay_instructions'] + 2*case['gap_instructions']*case['segments']*case['waves']
        predicted = cost+hop*hops+delay
        measured = record['makespan_cycles']
        rows.append(dict(case=case['name'],predicted_cycles=predicted,measured_cycles=measured,
            relative_error=(predicted-measured)/measured,absolute_percentage_error=100*abs(predicted-measured)/measured,
            predicted_limiting_resource=label))
    errors = [r['absolute_percentage_error'] for r in rows]
    return dict(status='ACCEPTED' if max(errors)<=20 else 'REJECTED',
        acceptance_rule='all withheld cases within 20%; declared before fitting; rejection is a finding',
        model='max_resource(setup * descriptors + cycles_per_KiB * payload_KiB) + hop_cost * H + declared delay instructions',
        training_cases=[r['case']['name'] for r in train],holdout_cases=[r['case']['name'] for r in holdout],
        coefficients=dict(setup_cycles=setup,cycles_per_KiB=cycles_per_kib,cycles_per_hop=hop),
        mean_absolute_percentage_error=statistics.mean(errors),maximum_absolute_percentage_error=max(errors),
        rmse_cycles=math.sqrt(statistics.mean((r['predicted_cycles']-r['measured_cycles'])**2 for r in rows)),
        limitations='Single-flow coefficients applied to a resource lower-envelope model. No claim of general compiler accuracy unless holdout error passes.',
        predictions=rows)
