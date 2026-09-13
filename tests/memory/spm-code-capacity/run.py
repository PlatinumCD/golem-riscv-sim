"""Code/data boundary and instruction working-set tests using one shared fixture."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import subprocess

HERE=Path(__file__).resolve().parent
DATA=65548
SPM=32768


def symbols(elf, nm):
    result={}
    for line in subprocess.check_output([str(nm),str(elf)],text=True).splitlines():
        fields=line.split()
        if len(fields)==3: result[fields[2]]=int(fields[0],16)
    return result


def build(directory, code, chunk):
    directory.mkdir(parents=True,exist_ok=True)
    with (directory/'build.log').open('w') as log:
        return subprocess.run(['make','-C',str(HERE),f'OUT={directory}/guest',
            f'CODE_BYTES={code}',f'CHUNK_BYTES={chunk}'],stdout=log,stderr=subprocess.STDOUT).returncode


def analyze(directory, code, chunk, layout):
    log=(directory/'simulation.log').read_text()
    assert 'CODE_DATA_PASS' in log and 'CODE_DATA_FAIL' not in log
    assert 'exited with exit status 0' in log
    boot=re.search(r'SCRATCHPAD_BOOT tile=0 bytes=(\d+) cycles=(\d+)',log)
    cache=re.search(r'INSTRUCTION_CACHE tile=0 (.*)',log)
    assert boot and cache, 'missing boot/cache counters'
    counters={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',cache[1])}
    assert counters['accesses']==counters['hits']+counters['misses']
    assert counters['unretired_fetches']==0
    assert layout['kernel_end']-layout['kernel']==code
    assert layout['__buffer_end']-layout['__buffer_start']==chunk
    assert layout['__bss_end']<=layout['__stack_bottom']
    assert layout['kernel_end']<=layout['__buffer_start']-32
    assert layout['__stack_top']==0x90000000+SPM
    assert layout['__stack_top']-layout['__stack_bottom']==2048
    profile=directory/'profile'
    starts={}; intervals={}
    with (profile/'tasks/tile-0.csv').open() as f:
        for row in csv.DictReader(f):
            key=(int(row['task_id']),int(row['execution_id']))
            tick=int(row['sim_time_ticks']); assert tick%1000==0
            if row['event']=='start':
                assert key not in starts and key not in intervals
                starts[key]=tick//1000
            else:
                assert row['event']=='finish' and key in starts
                intervals[key]=(starts.pop(key),tick//1000)
    assert not starts
    chunks=(DATA+chunk-1)//chunk
    assert set(intervals)=={(p,0) for p in (1,2,3)}|{(10+p,i) for p in range(3) for i in range(chunks)}
    traffic={p:{'memory_to_spm_bytes':0,'spm_to_memory_bytes':0} for p in range(4)}
    passes=[dict(fill_bytes=0,dma_bytes=0,elapsed_cycles=intervals[(10+p,0)][1]-intervals[(10+p,0)][0]) for p in range(3)]
    with (profile/'tile-0-scratchpad-beats.csv').open() as f:
        for row in csv.DictReader(f):
            client=int(row['client']); size=int(row['bytes']); address=int(row['offset']); cycle=int(row['service_cycle'])
            assert 0<size<=32 and 0<=address<address+size<=SPM
            if client==0:
                phase=next((p for p in (1,2,3) if intervals[(p,0)][0]<=cycle<intervals[(p,0)][1]),0)
                assert row['direction'] in ('read','write')
                traffic[phase]['memory_to_spm_bytes' if row['direction']=='write' else 'spm_to_memory_bytes']+=size
                if phase:
                    assert layout['__buffer_start']-0x90000000<=address
                    assert address+size<=layout['__buffer_end']-0x90000000
                else: assert cycle<intervals[(1,0)][0]
            if client in (0,9):
                for p in range(3):
                    a,b=intervals[(10+p,0)]
                    if a<=cycle<b: passes[p]['fill_bytes' if client==9 else 'dma_bytes']+=size
    assert traffic=={
        0:dict(memory_to_spm_bytes=int(boot[1]),spm_to_memory_bytes=0),
        1:dict(memory_to_spm_bytes=0,spm_to_memory_bytes=DATA),
        2:dict(memory_to_spm_bytes=DATA,spm_to_memory_bytes=DATA),
        3:dict(memory_to_spm_bytes=DATA,spm_to_memory_bytes=0)},traffic
    with (profile/'tile-0-summary.csv').open() as f:
        summary={r['metric']:int(r['value']) for r in csv.DictReader(f)}
    assert summary['physical_global_dma_submitted']==summary['physical_global_dma_completed']==4*chunks
    assert summary['scratchpad_dma_bytes']==4*DATA+int(boot[1])
    assert summary['network_packets']==0
    record=dict(status='PASS',code_body_bytes=code,chunk_bytes=chunk,spm_bytes=SPM,
        icache_bytes=8192,dataset_bytes=DATA,chunks=chunks,verified_words=DATA//4,
        spare_bytes_before_stack=layout['__stack_bottom']-layout['__bss_end'],
        phases=traffic,first_chunk_kernel_passes=passes,cache=counters,
        processing_cycles=intervals[(2,0)][1]-intervals[(2,0)][0])
    (directory/'results.json').write_text(json.dumps(record,indent=2)+'\n')
    return record


def execute(directory, code, chunk, nm, sst):
    assert build(directory,code,chunk)==0, str(directory/'build.log')
    layout=symbols(directory/'guest/tile.elf',nm)
    env=os.environ|dict(MITTENS_TEST_ELF=str(directory/'guest/tile.elf'),MITTENS_TEST_PROFILE=str(directory/'profile'))
    (directory/'profile/tasks').mkdir(parents=True)
    with (directory/'simulation.log').open('w') as log:
        result=subprocess.run([str(sst),str(HERE/'simulation.py')],env=env,
                              stdout=log,stderr=subprocess.STDOUT,timeout=180)
    assert result.returncode==0,str(directory/'simulation.log')
    return analyze(directory,code,chunk,layout)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=['capacity','cache'])
    parser.add_argument('output',type=Path)
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    nm=Path(os.environ['GOLEM_LLVM_DIR'])/'bin/llvm-nm'
    sst=Path(os.environ['GOLEM_INSTALL_ROOT'])/'sst-core/bin/sst'
    records=[]
    if args.mode=='capacity':
        for code in (4096,12288,20480):
            probe=out/f'probe-{code}'
            assert build(probe,code,32)==0,str(probe/'build.log')
            layout=symbols(probe/'guest/tile.elf',nm)
            maximum=(layout['__stack_bottom']-layout['__buffer_start']-32)//32*32
            for chunk in (maximum-32,maximum,maximum+32):
                directory=out/f'code-{code}-chunk-{chunk}'
                if chunk>maximum:
                    assert build(directory,code,chunk)!=0
                    assert 'buffer overlaps reserved stack' in (directory/'build.log').read_text()
                    assert not (directory/'simulation.log').exists()
                    record=dict(status='EXPECTED_REJECTION',code_body_bytes=code,chunk_bytes=chunk,
                                reason='buffer overlaps reserved stack',simulation_started=False)
                    (directory/'results.json').write_text(json.dumps(record,indent=2)+'\n')
                else:
                    record=execute(directory,code,chunk,nm,sst)
                    assert record['spare_bytes_before_stack']==maximum-chunk
                records.append(record); print(json.dumps(record),flush=True)
    else:
        for code in (4096,8192,12288,20480):
            record=execute(out/f'code-{code}',code,4096,nm,sst)
            passes=record['first_chunk_kernel_passes']
            assert passes[0]['fill_bytes']>=code
            record['dma_service_observed_during_first_traversal'] = passes[0]['dma_bytes'] > 0
            if code<8192: assert passes[1]['fill_bytes']<passes[0]['fill_bytes']/4
            if code>8192: assert passes[1]['fill_bytes']>=code*3/4 and passes[2]['fill_bytes']>=code*3/4
            records.append(record); print(json.dumps(record),flush=True)
    (out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'{args.mode}: {len(records)} cases PASS',flush=True)


if __name__=='__main__': main()
