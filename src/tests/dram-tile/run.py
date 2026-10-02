"""DRAM weight reads, multi-hop messages, direct receive programming and real MVMs."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import build, load_build_info

CASES = [dict(name='weights-two-destinations'),
         dict(name='delayed-single-slot', slots=1, delay=12000),
         dict(name='unaligned-payload', skew=7, packet=97),
         dict(name='slow-dram', cas=80, dram_queue=1),
         dict(name='one-channel', channels=1),
         dict(name='profile-on', profile=True)]
BASE, CAPACITY = 0x100000000, 1024*1024


def weights(tile):
    return [float(tile+r%3) if c==r else 0.25*tile if c==(r+3)%32 else 0.
            for r in range(32) for c in range(32)]


def rows(path):
    with path.open() as stream:
        return [{key: value if key=='event' else int(value) for key, value in row.items()}
                for row in csv.DictReader(stream)]


def stats(log, label):
    return sorted([json.loads(line[len(label)+1:]) for line in log.splitlines()
                   if line.startswith(label+' ')], key=lambda row: row['component'])


def prepare(trial, case, args):
    image=trial/'weights.bin'
    with image.open('wb') as stream:
        stream.truncate(CAPACITY)
        for tile in (1,3):
            stream.seek(tile*65536+case.get('skew',0))
            stream.write(struct.pack('<1024f',*weights(tile)))
    settings=dict(parameters=dict(spm_capacity_bytes=1024*1024,spm_banks=2,
        spm_bank_width=4,cpu_spm_banks=[0,1],router_spm_banks=[0,1],
        riscv_vector_length_bits=128,array_rows=32,array_cols=32,arrays_per_tile=1),
        cpu_parameters=dict(load_store_queue_depth=8,scalar_load_store_queue_depth=8,
                            analog_command_queue_depth=4),
        router_parameters=dict(memory_queue_depth=8,request_window=4,max_request_bytes=case.get('packet',256),
            posted_receive_slots_per_source=2,net_command_queue_depth=4,net_ticket_capacity=8),
        dram=dict(image=str(image),capacity_bytes=CAPACITY,t_cas_cycles=case.get('cas',12),
                  queue_depth=case.get('dram_queue',32)),
        transfers=[dict(transfer_id=100+tile,source_tile=0,destination_tile=tile,
            receive_base=0x90060000,slot_capacity=512,slot_count=case.get('slots',2)) for tile in (1,3)],
        qemu=str(args.qemu.resolve()),elfs=[])
    # Exercise the model default unless a case intentionally overrides it.
    if 'channels' in case:
        settings['dram']['channels']=case['channels']
    for tile in range(4):
        elf=trial/f'tile{tile}.elf'
        command=[str(args.compiler),'--target=riscv64-unknown-elf','-mcpu=golem-analog',
            '-march=rv64gcv_zvl128b_xgolemanalog','-mrvv-vector-bits=128','-mabi=lp64d',
            '-mcmodel=medany','-msmall-data-limit=0','-O2','-fno-vectorize','-fno-slp-vectorize',
            '-ffreestanding','-fno-builtin','-nostdlib','-static','-fuse-ld=lld',
            '-Wl,--no-relax,--build-id=none,--defsym=SPM_BYTES=1048576',
            f'-DTILE={tile}',f'-DSOURCE_SKEW={case.get("skew",0)}',f'-DDRAM_CAPACITY={CAPACITY}',
            f'-DRECEIVE_SLOTS={case.get("slots",2)}',f'-DDELAY={case.get("delay",0)}',
            '-T',str(SOURCE/'tests/network-instructions/scratchpad.ld'),
            str(SOURCE/'tests/riscv-qemu/start.S'),str(HERE/'guest.c'),'-o',str(elf)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=60)
        with (trial/'compile.log').open('a') as stream:
            stream.write(json.dumps(command)+'\n'+result.stdout+result.stderr)
        if result.returncode: raise RuntimeError(result.stderr)
        settings['elfs'].append(str(elf))
    (trial/'case.json').write_text(json.dumps(settings,indent=2)+'\n')
    return hashlib.sha256(image.read_bytes()).hexdigest()


def validate(trial, case, image_hash):
    topology=json.loads((trial/'topology.json').read_text())
    components={item['name']:item for item in topology['components']}
    def backend(component, slot):
        return next(child for child in component['subcomponents'] if child['slot_name']==slot)
    dram_backend=backend(components['net.tile0.dram_controller'],'backend')['params']
    assert int(dram_backend['channels'])==case.get('channels',2)
    assert int(dram_backend['max_requests_per_cycle'])==case.get('channels',2)
    for tile in range(4):
        spm_backend=backend(backend(components[f'net.tile{tile}.scratchpad'],'backendConvertor'),'backend')['params']
        assert int(spm_backend['spm_write_ports_per_bank'])==1
    log=(trial/'simulation.log').read_text()
    cpu=stats(log,'RISCV_STATS')
    network=stats(log,'NETWORK_INSTRUCTION_STATS')
    if not network: network=stats(log,'NETWORK_STATS')
    spm=stats(log,'MORDRED_SPM_STATS')
    dram=stats(log,'DRAM_TILE_STATS')
    assert len(cpu)==len(network)==len(spm)==4 and len(dram)==1
    assert all(row['idle'] for row in network+spm)
    assert network[0]['submitted']==network[0]['source_complete']==16
    assert network[0]['max_commands']<=4 and network[0]['max_packet_buffers']<=4
    assert spm[0]['bytes_read']==network[0]['descriptor_bytes'] and spm[0]['bytes_written']==0
    for tile in (1,3):
        assert network[tile]['received']==network[tile]['released']==8
        assert network[tile]['max_occupied_slots']<=case.get('slots',2)
        assert spm[tile]['bytes_written']==4096
    assert dram[0]['idle'] and dram[0]['bytes_read']==8192
    assert 1<dram[0]['max_pending_reads']<=8
    assert 0<dram[0]['busy_cycles']<=dram[0]['read_latency_cycles_sum']
    assert hashlib.sha256((trial/'weights.bin').read_bytes()).hexdigest()==image_hash
    for tile in range(4):
        assert cpu[tile]['scalar_load_store_queue_depth']==8 and cpu[tile]['load_store_queue_depth']==8
        assert cpu[tile]['lsq_enqueued']==cpu[tile]['lsq_completed']
        assert cpu[tile]['slq_enqueued']==cpu[tile]['slq_completed']
        with (trial/f'tile{tile}-spm.bin').open('rb') as stream:
            stream.seek(0xe0000)
            assert struct.unpack('<Q',stream.read(8))[0]==0x4452414d4f4b
            if tile in (1,3):
                stream.seek(0xa0000); output=struct.unpack('<32f',stream.read(128))
                matrix=weights(tile)
                expected=[sum(matrix[r*32+c]*(c+1)*0.5 for c in range(32)) for r in range(32)]
                assert list(output)==expected,(tile,output,expected)
    # Every payload byte was returned by timed DRAM service, with finite reads.
    issued={}; bytes_read=0
    trace=rows(trial/'net.tile0.router_spm-dram.csv')
    for row in trace:
        identity=row['request_id']
        if row['event']=='issue':
            assert identity not in issued and 0<row['bytes']<=64
            assert row['address']//64==(row['address']+row['bytes']-1)//64
            assert any(BASE+tile*65536+case.get('skew',0)<=row['address'] and
                       row['address']+row['bytes']<=BASE+tile*65536+case.get('skew',0)+4096 for tile in (1,3))
            issued[identity]=row
        else:
            start=issued.pop(identity)
            assert row['cycle']>start['cycle'] and (row['address'],row['bytes'])==(start['address'],start['bytes'])
            bytes_read+=row['bytes']
    assert not issued and bytes_read==8192
    assert len(trace)==2*dram[0]['requests']
    # The source CPU submits metadata; it cannot read/copy the DRAM payload.
    for row in rows(trial/'net.tile0.riscv-memory.csv'):
        assert not BASE<=row['address']<BASE+CAPACITY
    messages={tile:rows(trial/f'net.tile{tile}.router_spm-messages.csv') for tile in range(4)}
    assert not any(row['event']=='submit' for tile in (1,2,3) for row in messages[tile])
    leases=[]
    for tile in (1,3):
        ready={}; held={}; previous={}; done=[]
        for row in messages[tile]:
            if row['event']=='ready':
                ready[row['invocation_id']]=row['cycle']
            elif row['event']=='acquire':
                assert row['invocation_id'] in ready
                held[row['identity']]=dict(tile=tile,slot=row['offset'],invocation=row['invocation_id'],
                    ready=ready.pop(row['invocation_id']),acquire=row['cycle'],token=row['identity'])
            elif row['event']=='release':
                item=held.pop(row['identity']); item['release']=row['cycle']; done.append(item)
        assert not ready and not held and len(done)==8
        assert [item['invocation'] for item in done]==list(range(8))
        for item in sorted(done,key=lambda x:x['ready']):
            item['previous']=previous.get(item['slot'],-1)
            previous[item['slot']]=item['release']
            item['address']=0x90060000+512*item['slot']
            assert item['previous']<item['ready']<=item['acquire']<=item['release']
        committed={item['token']:[] for item in done}
        for row in rows(trial/f'net.tile{tile}.router_spm-spm.csv'):
            if row['event'] not in ('write_request','write_response'): continue
            address=0x90000000+row['address']
            owner=[item for item in done if item['address']<=address and address+row['bytes']<=item['address']+512
                   and item['previous']<row['cycle']<=item['ready']]
            assert len(owner)==1,('overwrite or early publication',tile,row)
            if row['event']=='write_response': committed[owner[0]['token']].append((address,row['bytes']))
        for item in done:
            address=item['address']
            for part,size in sorted(committed[item['token']]):
                assert part==address; address+=size
            assert address==item['address']+512
        # A memory fence before release must cover outstanding RVV input reads.
        for row in rows(trial/f'net.tile{tile}.riscv-memory.csv'):
            if not 0x90060000<=row['address']<0x90060000+512*case.get('slots',2): continue
            assert not row['write']
            assert any(item['address']<=row['address'] and row['address']+row['bytes']<=item['address']+512 and
                       item['acquire']<=row['cycle']<=item['release'] for item in done)
        leases.extend(done)
    source_complete=[row['cycle'] for row in messages[0] if row['event']=='source_complete']
    assert len(source_complete)==16 and source_complete[0]<min(item['release'] for item in leases)
    if case.get('delay'):
        delayed=next(item for item in leases if item['tile']==3 and item['invocation']==0)
        assert delayed['release']-delayed['acquire']>=case['delay']
        assert any(item['tile']==1 and item['release']<delayed['release'] for item in leases)
    profiles=list((trial/'profiles').glob('*dram-cycles.csv'))
    assert bool(profiles)==bool(case.get('profile'))
    return dict(passed=True,case=case['name'],dram=dram[0],end_cycles=[c['end_cycle'] for c in cpu],
                correct_mvm_tiles=[1,3],messages=16,payload_bytes=8192,
                ownership_checked=True,source_cpu_payload_copies=0)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',action='append',choices=[case['name'] for case in CASES])
    parser.add_argument('--build-info',type=Path)
    parser.add_argument('--compiler',type=Path,default=ROOT/'install/llvm/bin/clang')
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output=args.output.resolve(); args.output.mkdir(parents=True,exist_ok=True)
    info=load_build_info(args.build_info) if args.build_info else build(args.output/'components')
    results=[]
    for case in CASES:
        if args.case and case['name'] not in args.case: continue
        trial=args.output/case['name']; trial.mkdir()
        digest=prepare(trial,case,args)
        env=os.environ|dict(SST_LIB_PATH=info['plugin']+':'+info['library'],TILE_COMPONENT_OUTPUT=str(trial),
            TILE_CYCLE_PROFILE='1' if case.get('profile') else '0',PYTHONDONTWRITEBYTECODE='1')
        env.pop('TILE_COMPONENT_TRACE_START_TASK',None)
        env.pop('TILE_CYCLE_PROFILE_DIRECTORY',None)
        command=[info['sst'],'--num-threads=1',f'--output-json={trial/"topology.json"}',str(HERE/'simulation.py')]
        with (trial/'simulation.log').open('w') as log:
            child=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try: child.wait(timeout=180)
            finally:
                try: os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                child.wait()
        if child.returncode: raise RuntimeError((trial/'simulation.log').read_text()[-5000:])
        result=validate(trial,case,digest); results.append(result)
        (trial/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
        (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('PASS',case['name'],result['end_cycles'],flush=True)
    cases={result['case']:result for result in results}
    if 'weights-two-destinations' in cases:
        baseline=cases['weights-two-destinations']
        if 'profile-on' in cases: assert baseline['end_cycles']==cases['profile-on']['end_cycles']
        if 'slow-dram' in cases: assert max(cases['slow-dram']['end_cycles'])>max(baseline['end_cycles'])
    (args.output/'validation.json').write_text(json.dumps(dict(passed=True,cases=len(results)),indent=2)+'\n')


if __name__=='__main__': main()
