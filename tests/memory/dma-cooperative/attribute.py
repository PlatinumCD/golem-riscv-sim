"""Attribute measured retirements to fetched PCs and ELF inline debug frames.

Requires one-instruction fetch events. Fails closed on unaccounted retirements;
fetch counts alone are never presented as retired instruction counts.
"""
import argparse
import collections
import csv
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]

def rows(path):
    with path.open() as stream:
        yield from csv.DictReader(stream)

def classify(frames):
    text='\n'.join(frames)
    if 'globalDMAWaitForEvent' in text: return 'dma_event_wait_setup'
    if 'globalDMAQuery' in text or 'globalDMACompletionCommand' in text: return 'dma_poll'
    if 'globalDMAWait(' in text: return 'dma_blocking_wait_setup'
    if 'globalDMASubmit' in text: return 'dma_submit'
    if 'globalDMAAcknowledge' in text: return 'dma_acknowledge'
    if 'mesh_nic::trace_task' in text: return 'measurement_markers'
    if 'mesh_nic::wait_for_receive' in text: return 'mesh_wait_setup'
    if 'mesh_nic::status' in text: return 'mesh_status_poll'
    if 'mesh_nic::' in text: return 'mesh_handling'
    return 'runtime_bookkeeping'

def analyze(trial):
    result=json.loads((trial/'result.json').read_text())
    tasks=list(rows(trial/'tasks/tile-0.csv'))
    start=next(t for t in tasks if t['task_id']=='1' and t['event']=='start')
    end=next(t for t in tasks if t['task_id']=='1' and t['event']=='finish')
    low,high=int(start['retired_instructions']),int(end['retired_instructions'])
    counts=collections.Counter()
    previous_pc=None
    previous_count=0
    events=0
    intervals=[]
    for row in rows(trial/'profile/tile-0-instructions.csv'):
        current=int(row['instructions'])
        delta=max(0,min(current,high)-max(previous_count,low))
        if delta:
            assert previous_pc is not None and current-previous_count==1, row
            counts[previous_pc]+=delta
            intervals.append([int(row['tick'])-1000,int(row['tick']),previous_pc])
        if low<current<=high: events+=1
        previous_count=current
        if row['fetch_pc']: previous_pc=int(row['fetch_pc'])
        if current>high: break
    assert sum(counts.values())==high-low==result['instructions']
    elf=Path(result['guest'])/f'm{result["mode"]}-t0.elf'
    output=subprocess.check_output([str(ROOT/'install/llvm/bin/llvm-addr2line'),
        '-a','-f','-C','-i','-e',str(elf),*[hex(pc) for pc in counts]],text=True)
    locations={}
    address=None
    for line in output.splitlines():
        if re.fullmatch(r'0x[0-9a-fA-F]+',line):
            address=int(line,16); locations[address]=[]
        else:
            assert address is not None
            locations[address].append(line)
    assert set(counts)==set(locations)
    assert all(not all(s in ('??','??:0') for s in frames) for frames in locations.values())
    pc_categories={pc:classify(frames) for pc,frames in locations.items()}
    if result.get('work_iterations',0):
        symbols=subprocess.check_output([str(ROOT/'install/llvm/bin/llvm-nm'),str(elf)],text=True)
        bounds={line.split()[2]:int(line.split()[0],16) for line in symbols.splitlines()
                if 'dma_diagnostic_compute_' in line}
        begin=bounds['dma_diagnostic_compute_begin']; end=bounds['dma_diagnostic_compute_end']
        disassembly=subprocess.check_output([str(ROOT/'install/llvm/bin/llvm-objdump'),
                                            '-d','--no-show-raw-insn',str(elf)],text=True)
        for line in disassembly.splitlines():
            match=re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)',line)
            if not match: continue
            pc=int(match[1],16)
            if begin<=pc<end and pc in counts:
                pc_categories[pc]='compute_loop_control' if ('t0' in match[3] or match[2].startswith('b')) else 'useful_register_add'
    totals=collections.Counter()
    pcs=[]
    for pc,count in counts.items():
        category=pc_categories[pc]; totals[category]+=count
        pcs.append(dict(pc=hex(pc),retired=count,category=category,frames=locations[pc]))
    classified=[]
    for start_tick,end_tick,pc in intervals:
        category=pc_categories[pc]
        if classified and classified[-1][1]==start_tick and classified[-1][2]==category:
            classified[-1][1]=end_tick
        else: classified.append([start_tick,end_tick,category])
    if result.get('work_iterations',0):
        assert totals['useful_register_add']==8*result['work_iterations'],totals
        assert totals['compute_loop_control']==2*result['work_iterations'],totals
    report=dict(total_retired=high-low,attributed_retired=sum(totals.values()),
                categories=totals,region_bridge_events=events,pcs=pcs,
                instruction_issue_intervals=classified,
                interval_definition='One CPU cycle ending at the measured retirement boundary; device waits remain separate.')
    (trial/'attribution.json').write_text(json.dumps(report,indent=2)+'\n')
    print(trial.name,json.dumps(totals))
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path)
    args=parser.parse_args()
    results=json.loads((args.run/'results.json').read_text())
    for result in results:
        assert result['status']=='PASS' and result['instruction_trace']
        analyze(Path(result['directory']))

if __name__=='__main__': main()
