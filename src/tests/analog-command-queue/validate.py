"""Queue lifecycle, exact credit bounds, timed capture and completion audits."""
from collections import Counter
from bisect import bisect_right
import csv
import gzip
from pathlib import Path


def rows(path):
    if not path.exists(): path=Path(str(path)+'.gz')
    if not path.exists(): return
    with (gzip.open(path,'rt') if path.suffix=='.gz' else path.open()) as f:
        yield from csv.DictReader(f)


def numbers(row):
    return {k:int(v) if v and v.lstrip('-').isdigit() else v for k,v in row.items()}


def validate_asq(trial, case, result):
    depth = case.get('asq_depth', case.get('cpu_parameters', {}).get('analog_command_queue_depth', 0))
    trace=list(rows(trial/'riscv-asq.csv'))
    waits=list(rows(trial/'riscv-asq-waits.csv'))
    if not depth:
        assert not trace and not waits, 'Disabled ASQ emitted queue work'
        return dict(enabled=False, depth=0)
    cpu=result['cpu'];capacity=case['cpu_parameters']['analog_command_queue_bytes']
    active={};slots={};entries={};peak=peakbytes=0;bytecount=0;events=Counter();last=-1;lastqueue=0
    fields=('queue_token','slot','pc','operation','array','offset','count','register_mask')
    for raw in trace:
        r=numbers(raw);event=r['event'];c=r['cycle'];tok=r['token']
        assert c>=last;last=c;events[event]+=1
        if event=='stall':
            assert tok==r['queue_token']==r['pc']==0
        elif event=='enqueue':
            assert tok not in entries and r['slot'] not in slots and 0<=r['slot']<depth
            assert r['queue_token']>lastqueue;lastqueue=r['queue_token']
            assert r['operation'] in (0,1,3) and 0<=r['count']<=256
            mask=r['register_mask'];count=mask.bit_count();assert count in (1,2,4,8)
            reg=(mask & -mask).bit_length()-1
            assert reg%count==0 and mask==((1<<count)-1)<<reg and reg+count<=32
            e=dict(r,enqueue=c,issues=0,busy=0);entries[tok]=e;active[tok]=e;slots[r['slot']]=tok
            bytecount+=4*r['count'];peak=max(peak,len(active));peakbytes=max(peakbytes,bytecount)
        else:
            assert tok in active,(event,tok)
            e=active[tok];assert all(e[k]==r[k] for k in fields),(event,r,e)
            if event=='issue':
                assert 'accepted' not in e and e['issues']==e['busy'];e['issues']+=1;e['last_issue']=c
            elif event=='busy':
                assert 'accepted' not in e and e['issues']==e['busy']+1 and c>e['last_issue'];e['busy']+=1
            elif event in ('guaranteed','accepted'):
                assert 'accepted' not in e and e['issues']==e['busy']+1 and c>e['last_issue']
                e['accepted']=c;e['guaranteed']=event=='guaranteed'
            elif event=='captured':
                assert e['operation']!=3 and 'accepted' in e and 'captured' not in e and c>=e['accepted']
                e['captured']=c
            elif event in ('complete','error'):
                if event=='complete':
                    assert 'accepted' in e and c>=e['accepted']
                    assert e['operation']==3 or 'captured' in e
                else: assert not e.get('guaranteed',False)
                e['complete']=c;e['error']=event=='error'
                del active[tok];del slots[e['slot']];bytecount-=4*e['count']
            else: raise AssertionError(('Unknown ASQ event',r))
        assert r['occupancy']==len(active)<=depth and r['active_bytes']==bytecount<=capacity,(r,len(active),bytecount)
    assert not active and not slots and bytecount==0
    assert events['enqueue']==len(entries)==cpu['asq_enqueued']
    assert events['complete']==cpu['asq_completed']
    assert events['guaranteed']==cpu['asq_guaranteed'] and events['accepted']==cpu['asq_fallback']
    assert events['busy']==cpu['asq_busy'] and peak==cpu['asq_peak_occupancy'] and peakbytes==cpu['asq_peak_bytes']
    assert events['complete']+events['error']==events['enqueue']
    commands={};lastreads={};linkbytes=Counter()
    for raw in rows(trial/'arrays.csv'):
        r=numbers(raw);tok=r['token']
        if tok not in entries: continue
        e=entries[tok]
        assert (r['operation'],r['array'],r['element_offset'],r['element_count'])==(e['operation'],e['array'],e['offset'],e['count'])
        if r['event']=='start':
            assert tok not in commands;commands[tok]=dict(start=r['cycle']);assert r['cycle']>e['last_issue']
        elif r['event']=='complete':
            commands[tok]['complete']=r['cycle'];assert e['complete']==r['cycle']+1
        elif r['event'] in ('link_read','link_write'):
            linkbytes[tok]+=r['bytes'];assert r['cycle']>=commands[tok]['start']
            if r['event']=='link_read': lastreads[tok]=r['cycle']
    for tok,e in entries.items():
        if e['error']: continue
        assert tok in commands and 'complete' in commands[tok] and linkbytes[tok]==4*e['count']
        if e['operation']!=3 and e['count']:
            assert e['captured']==lastreads[tok]+2, ('Capture precedes physical delivery',e,lastreads[tok])
        assert e['enqueue']<=commands[tok]['start']<=commands[tok]['complete']<e['complete']
    # Reconstruct the exact release that satisfied every recorded wait. This
    # includes source-release waits that end before program delay completion.
    byslot={slot:[] for slot in range(depth)}
    for e in entries.values(): byslot[e['slot']].append(e)
    begins={slot:[e['enqueue'] for e in es] for slot,es in byslot.items()}
    totals=Counter();counts=Counter();last=-1
    for raw in waits:
        w=numbers(raw);start,end=w['start_cycle'],w['end_cycle'];reason=w['reason']
        assert last<=start<=end and reason in ('register','full','drain');last=end
        blockers=[]
        for slot,es in byslot.items():
            if not(w['mask'] & (1<<slot)): continue
            at=bisect_right(begins[slot],start)-1
            if at<0: continue
            e=es[at]
            if not(e['enqueue']<=start<e['complete']): continue
            release=e['captured'] if reason=='register' and e['operation']!=3 and 'captured' in e else e['complete']
            if release>start: blockers.append(release)
        assert blockers, ('Wait had no outstanding blocker',w)
        expected=min(blockers) if w['any'] else max(blockers)
        assert end==expected, ('ASQ wait released at wrong cycle',w,blockers)
        totals[reason]+=end-start;counts[reason]+=1
    assert sum(totals.values())==cpu['asq_stall_cycles'] and sum(counts.values())==events['stall']
    for raw in rows(trial/'riscv-tasks.csv'):
        c=int(raw['cycle']);assert not any(e['enqueue']<=c<e['complete'] for e in entries.values()),('Undrained marker',raw)
    assert not any(e['complete']>cpu['end_cycle'] for e in entries.values())
    return dict(enabled=True,depth=depth,byte_capacity=capacity,commands_checked=len(entries),
        peak_occupancy=peak,peak_bytes=peakbytes,events=dict(events),wait_cycles=dict(totals),
        wait_events=dict(counts),source_capture_before_command_completion=sum(
            e.get('captured',e['complete'])<e['complete'] for e in entries.values()),
        exact_capture_and_complete_timing_checked=True)
