"""Validate phase-specific DMA conservation, bounded storage and successful output checks."""
import csv
import json
from pathlib import Path
import re
import subprocess
import sys


def validate_dma_totals(traffic, summary, data_bytes, chunk_bytes, boot_bytes):
    assert traffic == {
        0:dict(memory_to_spm_bytes=boot_bytes,spm_to_memory_bytes=0),
        1:dict(memory_to_spm_bytes=0,spm_to_memory_bytes=data_bytes),
        2:dict(memory_to_spm_bytes=data_bytes,spm_to_memory_bytes=data_bytes),
        3:dict(memory_to_spm_bytes=data_bytes,spm_to_memory_bytes=0)}, traffic
    chunks=(data_bytes+chunk_bytes-1)//chunk_bytes
    assert summary['physical_global_dma_submitted']==4*chunks
    assert summary['physical_global_dma_completed']==4*chunks
    assert summary['scratchpad_dma_bytes']==4*data_bytes+boot_bytes
    assert summary['network_packets']==0
    return chunks


def check(root, data_bytes, chunk_bytes, nm):
    log = (root/'simulation.log').read_text()
    assert 'SPM_CHUNKING_PASS' in log and 'SPM_CHUNKING_FAIL' not in log
    assert 'exited with exit status 0' in log
    boot = re.search(r'SCRATCHPAD_BOOT tile=0 bytes=(\d+) cycles=(\d+)', log)
    assert boot, 'missing boot accounting'
    boot_bytes = int(boot[1])
    symbols = {}
    for line in subprocess.check_output([nm, str(root/'guest/tile.elf')], text=True).splitlines():
        fields = line.split()
        if len(fields)==3:
            symbols[fields[2]]=int(fields[0],16)
    base=0x90000000
    occupied = symbols['__heap_start']-base + symbols['__stack_top']-symbols['__stack_bottom']
    assert occupied <= 16384 < data_bytes
    buffer = next(value for key,value in symbols.items() if key.endswith('6windowE'))-base+32
    profile=root/'profile'
    with (profile/'tasks/tile-0.csv').open() as f:
        tasks=list(csv.DictReader(f))
    assert [(int(r['task_id']),r['event']) for r in tasks] == [
        (p,e) for p in (1,2,3) for e in ('start','finish')]
    intervals={p:(int(tasks[2*(p-1)]['sim_time_ticks'])//1000,
                  int(tasks[2*(p-1)+1]['sim_time_ticks'])//1000) for p in (1,2,3)}
    traffic={p:{'memory_to_spm_bytes':0,'spm_to_memory_bytes':0} for p in (0,1,2,3)}
    with (profile/'tile-0-scratchpad-beats.csv').open() as f:
        for beat in csv.DictReader(f):
            if int(beat['client'])!=0: continue
            cycle=int(beat['service_cycle']); size=int(beat['bytes']); address=int(beat['offset'])
            assert 0 < size <= 32 and 0 <= address < address+size <= 16384, beat
            phase=next((p for p,(a,b) in intervals.items() if a<=cycle<b),0)
            assert beat['direction'] in ('read','write')
            key='memory_to_spm_bytes' if beat['direction']=='write' else 'spm_to_memory_bytes'
            traffic[phase][key]+=size
            if phase:
                assert buffer <= address and address+size <= buffer+chunk_bytes, beat
            else:
                assert cycle < intervals[1][0], 'unattributed post-boot DMA'
    with (profile/'tile-0-summary.csv').open() as f:
        summary={r['metric']:int(r['value']) for r in csv.DictReader(f)}
    chunks=validate_dma_totals(traffic, summary, data_bytes, chunk_bytes, boot_bytes)
    return dict(status='PASS',data_bytes=data_bytes,spm_bytes=16384,
        chunk_bytes=chunk_bytes,chunks=chunks,last_chunk_bytes=data_bytes-(chunks-1)*chunk_bytes,
        allocated_spm_bytes=occupied,stack_reserved_bytes=2048,
        workload_dma_transfers=2*chunks,total_guest_dma_transfers=4*chunks,
        phases={name:dict(traffic[p],elapsed_cycles=intervals[p][1]-intervals[p][0])
                for p,name in ((1,'initialization'),(2,'processing'),(3,'verification'))},
        boot=traffic[0],verified_output_words=data_bytes//4)


if __name__=='__main__':
    root=Path(sys.argv[1])
    record=check(root,int(sys.argv[2]),int(sys.argv[3]),sys.argv[4])
    (root/'results.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record))
