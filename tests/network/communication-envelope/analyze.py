#!/usr/bin/env python3
"""Conservation and timestamp-based attribution, without additive-runtime fiction."""
from collections import Counter, defaultdict
import json
from pathlib import Path
import statistics
import sys

from contract import MeasurementError, csv_rows, elapsed, metric, required, save, summary, union_duration, write_csv

TICKS_PER_CYCLE = 1000


def valid_receive_order(frame, descriptor, schedule, dma_start, dma_end, authorized, consumed):
    """Full-frame arrival gates completion, not the first streaming burst."""
    return descriptor <= schedule <= dma_start <= dma_end <= authorized <= consumed and frame <= dma_end


def read_all(folder, pattern, *, needed=True):
    paths = sorted(folder.glob(pattern))
    if needed and not paths:
        raise MeasurementError(f'missing {pattern} in {folder}')
    return [row for p in paths for row in csv_rows(p)]


def task_intervals(rows):
    active, intervals = {}, {}
    for r in rows:
        key = tuple(int(r[k]) for k in ('tile_id','task_id','execution_id'))
        tick = int(r['sim_time_ticks'])
        if r['event'] == 'start':
            if key in active or key in intervals: raise MeasurementError(f'duplicate task {key}')
            active[key] = tick
        elif r['event'] == 'finish':
            if key not in active: raise MeasurementError(f'unmatched task {key}')
            start = active.pop(key); elapsed(start,tick); intervals[key] = (start,tick)
        else:
            raise MeasurementError(f'unknown task event: {r["event"]}')
    if active: raise MeasurementError(f'unfinished tasks: {active}')
    return intervals


def workload_observations(flows):
    """Outstanding messages are not NIC queue occupancy or router queue depth."""
    if not flows:
        return None
    events = []
    per_flow = defaultdict(list)
    for f in flows:
        events.extend(((f['offered_tick'], 1), (f['consumer_tick'], -1)))
        per_flow[f['flow']].append(f)
    outstanding = maximum = 0
    for _, delta in sorted(events):  # Completion before offer at identical ticks.
        outstanding += delta
        maximum = max(maximum, outstanding)
    if outstanding != 0:
        raise MeasurementError('message stream did not drain')
    rates = [sum(f['payload_bytes'] for f in rows) * 1000 /
             (max(f['consumer_tick'] for f in rows)-min(f['offered_tick'] for f in rows))
             for rows in per_flow.values()]
    return dict(max_outstanding_messages=maximum, outstanding_at_end=outstanding,
        drain_after_last_enqueue_cycles=max(0, max(f['consumer_tick'] for f in flows)-
                                              max(f['enqueue_finish_tick'] for f in flows))/1000,
        per_flow_lifetime_payload_rates=rates,
        lifetime_rate_jain_fairness=sum(rates)**2/(len(rates)*sum(r*r for r in rates)),
        offered_window_cycles=(max(f['offered_tick'] for f in flows)-min(f['offered_tick'] for f in flows))/1000,
        queue_depth=None,
        note='Guest-observed outstanding work and lifetime-rate fairness; not physical FIFO occupancy or steady-state fairness.')


def router_links(path, cycles):
    grouped = defaultdict(dict)
    for r in csv_rows(path):
        if r['ComponentName'].startswith('router_'):
            grouped[(r['ComponentName'],r['StatisticSubId'])][r['StatisticName']] = r
    links = []
    for (component,port), values in sorted(grouped.items()):
        if port not in ('east','west','north','south') and not port.startswith('local'): continue
        def val(name):
            row = values[name] if name in values else None
            return int(row['Sum.u64']) if row is not None else None
        busy, flits = val('output_link_busy_cycles'), val('flits_forwarded')
        util = busy/cycles if busy is not None else None
        if util is not None and util > 1.000001:
            raise MeasurementError(f'{component}/{port}: busy cycles exceed measured window')
        links.append(dict(component=component,port=port,wire_bytes=flits*4 if flits is not None else None,
            busy_cycles=busy, utilization=util,
            arbitration_stall_sum=val('switch_arbitration_stall_cycles'),
            credit_stall_sum=val('output_credit_stall_cycles'),
            scope='whole simulation; all network traffic belongs to measured workload'))
    if not links: raise MeasurementError('router measurements missing')
    return links


def analyze(trial):
    c = json.loads((trial/'case.json').read_text())
    serial = list((trial/'serial').glob('tile-*.log'))
    if len(serial) != len(c['active_tiles']): raise MeasurementError('missing guest serial logs')
    if any(p.read_text().count('ENVELOPE_PASS') != 1 or 'ENVELOPE_FAIL' in p.read_text() for p in serial):
        raise MeasurementError('guest validation incomplete')
    tasks = task_intervals(read_all(trial/'tasks','tile-*.csv'))
    run_windows = [required(tasks,(t,100,0)) for t in c['active_tiles']]
    start,finish = min(x[0] for x in run_windows),max(x[1] for x in run_windows)
    cycles = elapsed(start,finish)/TICKS_PER_CYCLE
    if cycles <= 0: raise MeasurementError('empty measured window')
    identities = set()
    for tile in c['active_tiles']:
        doc, vals = summary(trial/'profile'/f'tile-{tile}-summary.json')
        identities.add(doc['metadata']['implementation_id']['value'])
        if required(vals,'physical_global_dma_submitted') != 0: raise MeasurementError('unexpected global DMA')
    if len(identities)!=1 or None in identities: raise MeasurementError('missing/mixed binary identities')
    network = read_all(trial/'profile','tile-*-network.csv',needed=bool(c['flows']))
    dma = read_all(trial/'profile','tile-*-receive-dma.csv',needed=bool(c['flows']))
    boundaries = read_all(trial/'profile','tile-*-receive-boundaries.csv',needed=bool(c['flows']))
    raw_beats = read_all(trial/'profile','tile-*-scratchpad-beats.csv')
    beats = [{k:(v if k=='direction' else int(v)) for k,v in r.items()} for r in raw_beats]
    # DMA reservations can finish after the issuing CPU exits: use the global
    # receive-completion window, not the source's enqueue interval.
    tx_bytes = sum(r['bytes'] for r in beats if r['client'] in (1,2,3,4) and r['direction']=='read')
    rx_bytes = sum(r['bytes'] for r in beats if r['client'] in (5,6,7,8) and r['direction']=='write')
    expected = c['expected_payload_bytes']
    sent = sum(int(r['payload_words'])*4 for r in network if r['event']=='inject')
    arrived = sum(int(r['payload_words'])*4 for r in network if r['event']=='arrive')
    if (tx_bytes,rx_bytes,sent,arrived) != (expected,)*4:
        raise MeasurementError(f'payload conservation: expected {expected}; TX/RX/inject/arrive={(tx_bytes,rx_bytes,sent,arrived)}')
    occupied = set()
    for b in beats:
        key = (b['tile_id'],b['direction'],b['bank'],b['port'],b['service_cycle'])
        if key in occupied: raise MeasurementError(f'double-booked SPM port: {key}')
        occupied.add(key)
        if b['bank'] != (b['offset']//32)%8 or b['service_cycle'] < b['issue_cycle']:
            raise MeasurementError('SPM bank mapping/deadline mismatch')
    flows = []
    for f,(src,dst) in enumerate(c['flows']):
        for seq in range(c['segments']*c['waves']):
            route = 1000+f
            def match(r): return int(r['route_id'])==route and int(r['source'])==src and int(r['logical_iteration'])==seq
            events = [r for r in boundaries if match(r)]
            def event_tick(name):
                rows = [r for r in events if r['event']==name]
                if len(rows)!=1: raise MeasurementError(f'{route}/{seq}: expected one {name}, got {len(rows)}')
                return int(rows[0]['event_tick'])
            frame, descriptor = event_tick('frame-ready'),event_tick('descriptor')
            dmas = [r for r in dma if match(r) and r['event']=='complete']
            if not dmas: raise MeasurementError('missing RX DMA completion')
            schedule = min(int(r['schedule_tick']) for r in dmas)
            dma_start = min(int(r['dma_start_cycle'])*1000 for r in dmas)
            dma_end = max(int(r['dma_completion_cycle'])*1000 for r in dmas)
            authorized = max(int(r['event_tick']) for r in dmas)
            _, consumed = required(tasks,(dst,2000+f,seq))
            offered, enqueue_finish = required(tasks,(src,1000+f,seq))
            size = min(16384,c['payload']-(seq%c['segments'])*16384)
            if sum(int(r['words'])*4 for r in dmas)!=size: raise MeasurementError('RX descriptor bytes mismatch')
            # Streaming RX may begin consuming the first available bytes before
            # the complete frame arrives. Full-frame readiness is a completion
            # prerequisite, not a prerequisite for the first DMA burst.
            if not valid_receive_order(frame,descriptor,schedule,dma_start,dma_end,authorized,consumed):
                raise MeasurementError(f'RX timestamps reversed: {route}/{seq}')
            elapsed(offered,consumed)
            flows.append(dict(flow=f,sequence=seq,source=src,destination=dst,payload_bytes=size,
                offered_tick=offered,enqueue_finish_tick=enqueue_finish,frame_ready_tick=frame,
                descriptor_tick=descriptor,dma_schedule_tick=schedule,dma_start_tick=dma_start,
                dma_complete_tick=dma_end,authorized_tick=authorized,consumer_tick=consumed,
                latency_cycles=(consumed-offered)/1000,
                receiver_arm_delay_cycles=max(0,descriptor-frame)/1000,
                descriptor_wait_for_data_cycles=max(0,frame-descriptor)/1000,
                ready_to_schedule_cycles=max(0,schedule-max(frame,descriptor))/1000,
                dma_start_before_full_frame_cycles=max(0,frame-dma_start)/1000,
                scheduled_queue_cycles=(dma_start-schedule)/1000,
                dma_span_cycles=(dma_end-dma_start)/1000,
                authorization_to_guest_cycles=(consumed-authorized)/1000))
    links = router_links(trial/'router-statistics.csv',cycles)
    center = c['rvv_tile'] if c['rvv_tile']>=0 else (12 if 12 in c['active_tiles'] else c['active_tiles'][0])
    # Validate all recorded reservations above, but measure CPU requests only
    # inside their tile's run markers. Final buffer verification is excluded.
    measured_beats = [b for b in beats if
        (b['client'] == -1 and tasks[(b['tile_id'],100,0)][0] <= b['service_cycle']*1000 < tasks[(b['tile_id'],100,0)][1]) or
        (b['client'] != -1 and start <= b['service_cycle']*1000 < finish)]
    clients = {}
    client_beats = {}
    compute_window = tasks[(center,200,0)] if (center,200,0) in tasks else None
    for name,ids in (('rvv',(-1,)),('tx',(1,2,3,4)),('rx',(5,6,7,8))):
        selected = [b for b in measured_beats if b['tile_id']==center and b['client'] in ids and
                    (name!='rvv' or (b['offset']==0 and b['bytes']==32 and b['direction']=='read'
                     and compute_window and compute_window[0]<=b['service_cycle']*1000<compute_window[1]))]
        client_beats[name] = selected
        if not selected:
            clients[name] = None
            continue
        first,last = min(b['service_cycle'] for b in selected),max(b['service_cycle']+1 for b in selected)
        byte_count = sum(b['bytes'] for b in selected)
        clients[name] = dict(bytes=byte_count,first_cycle=first,last_cycle=last,
            span_cycles=last-first,B_per_span_cycle=byte_count/(last-first),
            occupied_cycles=len({b['service_cycle'] for b in selected}))
    active = [v for v in clients.values() if v is not None]
    common_start = max(v['first_cycle'] for v in active) if active else None
    common_end = min(v['last_cycle'] for v in active) if active else None
    common_cycles = max(0,common_end-common_start) if active else 0
    simultaneously = 0
    if common_cycles:
        sets = []
        for name, rows in client_beats.items():
            if clients[name] is None: continue
            selected = [b for b in rows if common_start<=b['service_cycle']<common_end]
            clients[name]['common_window_bytes'] = sum(b['bytes'] for b in selected)
            clients[name]['common_window_B_per_cycle'] = clients[name]['common_window_bytes']/common_cycles
            sets.append({b['service_cycle'] for b in selected})
        simultaneously = len(set.intersection(*sets))
    if c['rvv_iterations'] and (clients['rvv'] is None or clients['rvv']['bytes'] != c['rvv_iterations']*32):
        raise MeasurementError('RVV beat count does not match declared work')
    per_cycle = Counter((b['tile_id'],b['service_cycle'],b['direction']) for b in measured_beats)
    bank_phases = {}
    for name,rows in client_beats.items():
        bank_phases[name] = dict(Counter(b['bank'] for b in rows))
    # Resource sums are explicitly not elapsed intervals, and unavailable
    # RX frontend arbitration remains null rather than borrowing router stalls.
    result = dict(schema='golem.communication-envelope.case',schema_version=2,
        status='PASS',implementation_id=next(iter(identities)),case=c,center_tile=center,
        start_tick=start,finish_tick=finish,makespan_cycles=cycles,
        payload_bytes=expected,payload_B_per_cycle=expected/cycles,
        metrics={
            'elapsed':metric(finish-start,'tick','elapsed_interval','first run start to last guest completion'),
            'rx_frontend_serialization':metric(None,'cycle','resource_stall_sum','RX descriptor scheduler only',reason='no dedicated cause counter; see timestamp attribution, not router stalls'),
            'rx_dma_interval_sum':metric(sum(x['dma_span_cycles'] for x in flows),'cycle','resource_interval_sum','sum over completed descriptor service spans; may overlap other resources'),
            'rx_dma_busy_union':metric(union_duration([(int(r['dma_start_cycle']),int(r['dma_completion_cycle'])) for r in dma if r['event']=='complete' and int(r['tile_id'])==center]),'cycle','interval_union','center RX scheduled service including setup'),
            'spm_bank_wait_sum':metric(sum(b['service_cycle']-b['issue_cycle'] for b in measured_beats),'cycle','request_wait_sum','SPM port reservation waits, measured client work across all tiles; post-run CPU verification excluded'),
        },
        conservation=dict(tx_spm_bytes=tx_bytes,rx_spm_bytes=rx_bytes,injected_payload_bytes=sent,received_payload_bytes=arrived,
            completed_descriptors=len(flows),expected_descriptors=len(c['flows'])*c['segments']*c['waves']),
        clients=clients,common_window=dict(start_cycle=common_start,finish_cycle=common_end,
            cycles=common_cycles,all_clients_service_same_cycle=simultaneously,
            note='intersection of service spans; occupancy and same-cycle count prove actual service, not continuous peak demand'),
        bank_requests=bank_phases,max_reads_same_cycle=max((v for (_,_,d),v in per_cycle.items() if d=='read'),default=0),
        max_writes_same_cycle=max((v for (_,_,d),v in per_cycle.items() if d=='write'),default=0),
        flows=flows,links=links,
        workload=workload_observations(flows),
        attribution_note='RX timestamps are separate from physical-link arbitration. Per-resource sums are not a partition of elapsed time.')
    if flows:
        write_csv(trial/'flows.csv',flows)
        result['latency'] = dict(mean=statistics.mean(f['latency_cycles'] for f in flows),
            maximum=max(f['latency_cycles'] for f in flows),
            minimum=min(f['latency_cycles'] for f in flows))
    else:
        result['latency'] = None
    save(trial/'record.json',result)
    print(f'PASS {c["name"]}: {cycles:g} cycles; {len(flows)} descriptors; {expected} bytes; common window {common_cycles} cycles')
    return result


if __name__ == '__main__':
    analyze(Path(sys.argv[1]))
