"""Produce a compact evidence report from completed diagnostic runs."""
import argparse
import collections
import html
import json
from pathlib import Path
import statistics

def load(path):
    complete=json.loads((path/'complete.json').read_text())
    records=json.loads((path/'results.json').read_text())
    assert complete['status']=='PASS' and complete['cases']==len(records)
    assert len(records)==len(json.loads((path/'plan.json').read_text()))
    assert all(r['status']=='PASS' for r in records)
    return records

def table(records):
    groups=collections.defaultdict(list)
    for r in records:
        groups[(r['rounds'],r['mode'],r['quantum'],r['interval'],r.get('work_iterations',0))].append(r)
    out=['| Mesh rounds | Policy | Quantum | Poll interval | Compute iterations | Repeats | Cycles mean [min,max] | Instructions mean | Host seconds mean [min,max] |',
         '|---:|---|---:|---:|---:|---:|---|---:|---|']
    names={0:'blocking',1:'polling',2:'polling-16',3:'event-driven'}
    for (rounds,mode,q,interval,work),rs in sorted(groups.items()):
        cycles=[r['elapsed_cycles'] for r in rs]; wall=[r['host_wall_seconds'] for r in rs]
        out.append(f'| {rounds} | {names[mode]} | {q} | {interval} | {work} | {len(rs)} | '
                   f'{statistics.mean(cycles):.1f} [{min(cycles):.0f},{max(cycles):.0f}] | '
                   f'{statistics.mean(r["instructions"] for r in rs):.1f} | '
                   f'{statistics.mean(wall):.3f} [{min(wall):.3f},{max(wall):.3f}] |')
    return '\n'.join(out)

def timeline(result):
    directory=Path(result['directory'])
    data=json.loads((directory/'timeline.json').read_text())
    attribution=json.loads((directory/'attribution.json').read_text())
    begin,end=data['cpu_region']
    low=min([begin]+[int(r['injection_tick']) for r in data['requests']])
    high=max([end]+[int(r['event_tick']) for r in data['peer_received']])
    def x(t): return 175+850*(t-low)/(high-low)
    colors={'dma_poll':'#cb5b45','mesh_status_poll':'#dc9560','mesh_handling':'#2d7f9e',
            'runtime_bookkeeping':'#8995a5','measurement_markers':'#bdc3ca',
            'dma_event_wait_setup':'#548b65','dma_blocking_wait_setup':'#548b65',
            'dma_submit':'#8064a2','dma_acknowledge':'#8064a2',
            'useful_register_add':'#26927d','compute_loop_control':'#76b6a0'}
    parts=['<svg viewBox="0 0 1080 275" role="img" aria-label="Measured CPU, RAM DMA, and mesh timeline">']
    def rect(a,b,y,color,title):
        parts.append(f'<rect x="{x(a):.3f}" y="{y}" width="{max(.12,x(b)-x(a)):.3f}" height="18" fill="{color}"><title>{html.escape(title)}</title></rect>')
    for y,label in [(45,'CPU instructions'),(80,'CPU blocked'),(115,'RAM service'),(150,'Requests in flight'),(185,'Replies in flight')]:
        parts.append(f'<text x="5" y="{y+13}" font-size="13">{label}</text>')
        parts.append(f'<line x1="175" y1="{y+19}" x2="1025" y2="{y+19}" stroke="#ddd"/>')
    for a,b,category in attribution['instruction_issue_intervals']:
        rect(a,b,45,colors.get(category,'#888'),category)
    for wait in data['cpu_waits']:
        if wait['reason'] in ('scratchpad-dma-wait','scratchpad-dma-or-mesh-wait','nic-receive-wait'):
            rect(int(wait['start_tick']),int(wait['finish_tick']),80,'#e1b851',wait['reason'])
    dma=data['dma']
    rect(int(dma['arrival_cycle'])*1000,int(dma['service_start_cycle'])*1000,115,'#ddd','RAM queued')
    rect(int(dma['service_start_cycle'])*1000,int(dma['completion_cycle'])*1000,115,'#6955a0','RAM service')
    for row in data['requests']: rect(int(row['injection_tick']),int(row['event_tick']),150,'#3477a2','request in flight')
    for row in data['peer_received']: rect(int(row['injection_tick']),int(row['event_tick']),185,'#28836a','reply in flight')
    for i in range(6):
        tick=low+(high-low)*i/5
        parts.append(f'<text x="{x(tick):.1f}" y="230" text-anchor="middle" font-size="12">{(tick-begin)/1000:.0f}</text>')
    parts.append('<text x="600" y="258" text-anchor="middle" font-size="13">Cycles relative to CPU measured-region start</text></svg>')
    return ''.join(parts)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    names=('baseline','corrected','compute','event','attribution','frequency_attribution','compute_attribution','host','router')
    for name in names:
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    studies={name:load(getattr(args,name)) for name in names}
    for before,after in zip(studies['baseline'],studies['corrected']):
        assert all(before[k]==after[k] for k in ['rounds','mode','quantum','interval','repeat','guest_sha256'])
    text=['# DMA and mesh waiting diagnostics','',
          'This report separates host execution time, guest instructions, elapsed simulated cycles, and resource activity. '
          'Resource stalls are not added together as runtime. Missing measurements remain null.',
          '', '## Evidence boundaries','',
          '- Baseline and corrected runs reuse identical guest ELFs; only the simulator build changes.',
          '- Host times include verification after the timed interval. Initial sweeps ran alongside builds or other simulations; do not interpret their wall-time ratios as isolated host speedups.',
          '- CPU instruction categories come from retirement deltas matched to fetched PCs, ELF disassembly, and inline debug frames—not from fetch counts alone.',
          '- CPU blocked intervals, RAM service, and network transit overlap and remain separate in the timelines.',
          '- These are synthetic architectural diagnostics, not ResNet inference results.','']
    for name,records in studies.items():
        text += ['## '+name.title(),'',f'Evidence: `{getattr(args,name).resolve()}`','',table(records),'']
    (args.output/'measurements.md').write_text('\n'.join(text)+'\n')
    figures=[]
    illustrated=studies['attribution']+[r for r in studies['compute_attribution'] if r.get('work_iterations')==2048]
    for r in sorted(illustrated,key=lambda r:(r.get('work_iterations',0),r['rounds'],r['mode'])):
        directory=Path(r['directory']); attribution=json.loads((directory/'attribution.json').read_text())
        assert attribution['total_retired']==attribution['attributed_retired']==r['instructions']
        policy={0:'Blocking wait',1:'Polling',3:'Event-driven wait'}[r['mode']]
        workload='Idle DMA wait' if r['rounds']==0 else f'{r["rounds"]} mesh request/reply exchanges'
        if r.get('work_iterations',0):
            workload=f'{r["work_iterations"]:,} register-compute iterations plus DMA'
        title=f'{workload} — {policy}'
        counts='<table><tr><th>Instruction category</th><th>Retired instructions</th></tr>'+''.join(
            f'<tr><td>{html.escape(k.replace("_"," ").capitalize())}</td><td>{v:,}</td></tr>'
            for k,v in sorted(attribution['categories'].items()))+'</table>'
        figures.append(f'<section><h2>{title}</h2>{timeline(r)}{counts}</section>')
    (args.output/'timelines.html').write_text('<!doctype html><meta charset="utf-8"><title>Measured DMA and mesh timelines</title>'
        '<style>body{font:15px system-ui;background:white;color:#243142;max-width:1100px;margin:35px auto;padding:0 20px}h1{font-size:24px}h2{font-size:17px}section{margin:30px 0;border-top:1px solid #ddd}svg{width:100%;height:auto}table{font-size:12px;border-collapse:collapse}th,td{padding:4px 18px 4px 0;text-align:left;border-bottom:1px solid #eee}td:last-child{text-align:right;font-variant-numeric:tabular-nums}</style>'
        '<h1>Measured DMA and mesh timelines</h1><p>Hover over intervals for labels. CPU colors distinguish DMA polling (red), mesh polling (orange), mesh handling (blue), useful register adds (green), and bookkeeping (gray). Blocked time is yellow; RAM service is purple. These concurrent rows must not be added together. In-flight packets include pipeline and propagation time, not just link service.</p>'+''.join(figures))
    print(args.output)

if __name__=='__main__': main()
