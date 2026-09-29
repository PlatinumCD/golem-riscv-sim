#!/usr/bin/env python3
"""Directed ASQ hazards, credit pressure, precise traps and deterministic replay."""
import argparse
from collections import Counter
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]
ROOT=SOURCE.parent
sys.path.insert(0,str(ROOT/'source_new'))
from configuration import resolve
from validate import validate_asq, rows, numbers
spec=importlib.util.spec_from_file_location('existing_lsq_tests',ROOT/'source_new/tests/analog-register-dependencies/run.py')
existing=importlib.util.module_from_spec(spec);spec.loader.exec_module(existing)
sha=existing.digest;write=existing.dump


def compile_guest(output,vlen,compiler=None,guest_source=None):
    output=Path(output).resolve()
    compiler=Path(compiler or ROOT/'install/llvm/bin/clang').resolve()
    guest_source=Path(guest_source or HERE/'directed.S').resolve()
    output.mkdir(parents=True,exist_ok=True);n=vlen//4
    data=output/'data.S';lines=['.section .rodata,"a",@progbits']
    for name,values in [('weights',(float(i==j) for i in range(n) for j in range(n))),
                        ('weights_two',(2.*(i==j) for i in range(n) for j in range(n))),
                        ('input_data',range(1,n+1)),('noise_data',range(1000,1000+n))]:
        words=[str(struct.unpack('<I',struct.pack('<f',v))[0]) for v in values]
        lines+=['.balign 4096','.global '+name,name+':']
        lines+=['.word '+','.join(words[i:i+16]) for i in range(0,len(words),16)]
    data.write_text('\n'.join(lines)+'\n')
    source=SOURCE;elf=output/'guest.elf'
    command=[str(compiler),'--target=riscv64-unknown-elf','-mcpu=golem-analog','-fuse-ld=lld',
        '-mabi=lp64d','-mcmodel=medany','-msmall-data-limit=0','-nostdlib','-static',
        '-Wl,--no-relax','-Wl,--build-id=none',f'-DVLEN_BITS={vlen}',
        '-T',str(source/'tests/riscv-qemu/scratchpad.ld'),str(source/'tests/riscv-qemu/start.S'),
        str(source/'tests/array-pipeline/measurement.S'),str(guest_source),str(data),'-o',str(elf)]
    p=subprocess.run(command,capture_output=True,text=True,timeout=90)
    (output/'build.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
    (output/'guest.asm').write_text(subprocess.check_output([str(compiler.parent/'llvm-objdump'),'-d',str(elf)],text=True))
    symbols={}
    for line in subprocess.check_output([str(compiler.parent/'llvm-nm'),'--defined-only',str(elf)],text=True).splitlines():
        x=line.split()
        if len(x)==3:symbols[x[2]]=int(x[0],16)
    g=dict(elf=str(elf),sha256=sha(elf),vlen=vlen,symbols=symbols,command=command,
        guest_source_sha256=sha(guest_source),compiler_sha256=sha(compiler))
    write(output/'build.json',g);return g


def expected_memory(trial,case):
    lanes=case['vlen']//32;n=lanes*8;x=list(range(1,n+1));noise=list(range(1000,1000+n))
    words=lambda xs:b''.join(struct.pack('<f',v) for v in xs)
    short=noise.copy();short[0]=1;short[-lanes:]=x[:lanes]
    expected={0:bytes(n*4),1:words(x),2:bytes(n*4),
        3:b''.join(struct.pack('<I',struct.unpack('<I',struct.pack('<f',v))[0]+1) for v in x),
        4:words(noise),5:words(x),6:words(noise),7:words(x),8:words(short),
        9:words(x[:lanes//2]+noise[lanes//2:lanes]),10:words(x),11:words([2*v for v in noise]),
        12:words(noise),13:words([2*v for v in x]),14:words(noise),15:words(x),16:words(noise),
        17:words(noise),18:words(x)}
    memory=(trial/'scratchpad.bin').read_bytes()
    for slot,wanted in expected.items():
        off=0x110000+4096*slot;actual=memory[off:off+len(wanted)]
        assert actual==wanted,('Output mismatch',slot,
            [(i,a,b) for i,(a,b) in enumerate(zip(struct.unpack('<%dI'%(len(actual)//4),actual),struct.unpack('<%dI'%(len(wanted)//4),wanted))) if a!=b][:8])
        assert memory[off+len(wanted):off+4096]==bytes(4096-len(wanted)),('Tail overwrite',slot)
    assert struct.unpack_from('<I',memory,0x1e0000)[0]==2
    assert struct.unpack_from('<QQ',memory,0x1e0008)==(2,case['symbols']['target_p15'])
    return sum(len(b)//4 for b in expected.values())


def canonical_traces(trial):
    files={p.name:list(rows(p)) for p in trial.glob('*.csv')}
    analog=[rs for name,rs in files.items() if name.startswith('array') or name=='riscv-asq.csv']
    mapping={t:i+1 for i,t in enumerate(sorted({int(r['token']) for rs in analog for r in rs if r.get('token') and int(r['token'])}))}
    out={}
    for name,rs in files.items():
        if name.startswith('array') or name=='riscv-asq.csv':
            rs=[{k:(mapping[int(v)] if k=='token' and int(v) else v) for k,v in r.items()} for r in rs]
        out[name]=hashlib.sha256(json.dumps(rs,separators=(',',':')).encode()).hexdigest()
    return out


def validate(trial,case):
    log=(trial/'simulation.log').read_text();cpu=existing.stats(log,'RISCV_STATS');arrays=existing.stats(log,'ARRAY_STATS')
    entries,stalls=existing.check_lsq(trial,case,cpu)
    checked=expected_memory(trial,case)
    assert cpu['memory_requests']==cpu['completed_requests']
    assert arrays['accepted']==arrays['completed']==cpu['analog_commands']
    assert arrays['errors']==1 and arrays['mvms']==13,arrays
    result=dict(cpu=cpu,arrays=arrays,lsq_stalls=stalls,outputs_checked=checked)
    result['asq']=validate_asq(trial,case,result)
    markers=[numbers(r) for r in rows(trial/'riscv-tasks.csv')];assert len(markers)==32
    fetches=[numbers(r) for r in rows(trial/'riscv-icache.csv')]
    asq=[numbers(r) for r in rows(trial/'riscv-asq.csv')]
    timeline=[numbers(r) for r in rows(trial/'arrays.csv')]
    memory_events=[numbers(r) for r in rows(trial/'riscv-memory.csv')]
    phases={}
    for phase in range(1,17):
        pair=[r for r in markers if r['task_id']==phase];assert [r['event'] for r in pair]==['start','finish']
        lo,hi=[r['cycle'] for r in pair];phases[str(phase)]=dict(start=lo,end=hi,cycles=hi-lo)
        for r in pair:
            assert r['memory_requests']==r['completed_requests'] and r['lsq_enqueued']==r['lsq_completed']
        def fetch(symbol,pc=None):
            address=case['symbols'][symbol] if pc is None else pc
            f=[r['cycle'] for r in fetches if r['event'] in ('hit','miss') and r['address']==address and lo<=r['cycle']<=hi]
            assert len(f)==1,(symbol,f);return f[0]
        if not case['asq_depth'] or phase in (9,10,13):continue
        command=[r for r in asq if r['event']=='enqueue' and r['pc']==case['symbols'][f'command_p{phase}']]
        assert len(command)==1,(phase,command)
        events={r['event']:r for r in asq if r['token']==command[0]['token'] and r['event']!='stall'}
        if phase==16:
            if case['depth']>1:
                oldstores=[e for e in entries if e['pc']==case['symbols']['issue_p16_store']]
                assert len(oldstores)==8 and all(e['write'] for e in oldstores)
                assert command[0]['cycle']<max(e['complete'] for e in oldstores),'Captured older stores did not overlap ASQ'
            continue
        target_pc=case['symbols'][f'target_p{phase}']
        if phase in (8,14):
            # These labels precede the save macro's independent address setup.
            # Time the actual dependent vector store, not its scalar LUI.
            stores={r['pc'] for r in memory_events if r['event']=='issue' and r['vector'] and r['write']
                and lo<=r['cycle']<=hi and target_pc<=r['pc']<target_pc+32}
            assert len(stores)==1,(phase,stores);target_pc=stores.pop()
        target=fetch(f'target_p{phase}',target_pc)
        info=phases[str(phase)];info.update(target_fetch=target,complete=events['complete']['cycle'])
        if phase in (1,2,6):
            capture=events['captured']['cycle'];assert capture<=target
            info['captured']=capture
            if phase==1:assert target<events['complete']['cycle'],'Source remained pinned through programming delay'
        elif phase in (3,4,5,7,8,14):assert events['complete']['cycle']<=target,('Output hazard',phase,events,target)
        elif phase in (11,15):
            handler=fetch('trap_handler');assert events['complete']['cycle']<=handler
            assert events['captured']['cycle']<=target<events['complete']['cycle'],('Fault did not exercise captured outstanding ASQ',events,target)
            info['handler_fetch']=handler
        elif phase==12:assert events['complete']['cycle']<=target,'Fence failed to drain queue'
    if case['asq_depth']:
        assert result['asq']['peak_occupancy']==min(case['asq_depth'],case['cpu_parameters']['analog_command_queue_bytes']//(case['vlen']))
        assert result['asq']['wait_cycles'].get('full',0)>0,'Credit-pressure test did not fill queue'
        zero=[r for r in asq if r['event']=='enqueue' and phases['13']['start']<=r['cycle']<=phases['13']['end']]
        assert [r['operation'] for r in zero]==[0,1,3] and all(r['count']==0 for r in zero)
        assert all(any(s['event']=='captured' and s['token']==r['token'] for s in asq) for r in zero[:2])
    computes=Counter(r['array'] for r in timeline if r['event']=='start' and r['operation']==2)
    assert computes==Counter({0:11,1:2}) and arrays['peak_active_arrays']==2
    result.update(phases=phases,memory_sha256=sha(trial/'scratchpad.bin'),canonical_trace_hashes=canonical_traces(trial))
    return result


def add_model_arguments(parser):
    parser.add_argument('--build-info',type=Path,default=ROOT/'build/source_new-analog-queue/build.json')
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--baseline-build-info',type=Path)
    parser.add_argument('--baseline-qemu',type=Path)
    parser.add_argument('--compiler',type=Path,default=ROOT/'install/llvm/bin/clang')


def load_model(variant,build_info,qemu):
    build_info,qemu=Path(build_info).resolve(),Path(qemu).resolve()
    build=json.loads(build_info.read_text())
    if variant=='candidate':
        for name,expected in build['source_sha256'].items():
            assert sha(name)==expected,('Build input changed; rebuild before testing',name)
    # An explicitly supplied historical baseline may predate the current source;
    # record its immutable manifest and actual binary identities without silently
    # substituting a different executable or rebuilding it.
    return dict(variant=variant,source=str(SOURCE),build=build,build_info=str(build_info),
        build_info_sha256=sha(build_info),qemu=str(qemu),qemu_sha256=sha(qemu),
        plugin_sha256=sha(Path(build['plugin'])/'libtilecomponents.so'),
        current_source_verified=variant=='candidate')


def models_from_arguments(args,parser,candidate=True):
    if bool(args.baseline_build_info)!=bool(args.baseline_qemu):
        parser.error('--baseline-build-info and --baseline-qemu must be supplied together')
    models={}
    if candidate:models['candidate']=load_model('candidate',args.build_info,args.qemu)
    if args.baseline_build_info:
        models['baseline']=load_model('baseline',args.baseline_build_info,args.baseline_qemu)
    return models


def model_identities_unchanged(models):
    for model in models.values():
        assert sha(model['qemu'])==model['qemu_sha256'],'QEMU changed during test'
        assert sha(Path(model['build']['plugin'])/'libtilecomponents.so')==model['plugin_sha256'],'Plugin changed during test'
        assert sha(model['build_info'])==model['build_info_sha256'],'Build manifest changed during test'


def simulate(output,name,guest,variant,vlen,asq_depth,lsq_depth,bytecap,budget,model):
    trial=Path(output)/name;trial.mkdir()
    build=model['build']
    cpu=dict(load_store_queue_depth=lsq_depth,instruction_budget=budget,host_timeout_seconds=120)
    if variant!='baseline':cpu.update(analog_command_queue_depth=asq_depth,analog_command_queue_bytes=bytecap)
    case=dict(name=name,variant=variant,asq_depth=asq_depth,depth=lsq_depth,budget=budget,**guest,
        component_source=model['source'],qemu=model['qemu'],qemu_sha256=model['qemu_sha256'],
        plugin_sha256=model['plugin_sha256'],build_info=model['build_info'],
        parameters=resolve(dict(riscv_vector_length_bits=vlen,array_rows=vlen//4,array_cols=vlen//4,
            arrays_per_tile=2,spm_banks=8,array_pipeline_enabled=True,cost_per_array_program_cycles=256,
            cost_per_mvm_cycles=100,array_program_delay_scope='per_command',array_inflight_bytes=128)),cpu_parameters=cpu)
    assert sha(case['elf'])==case['sha256'],'Guest changed after compilation'
    write(trial/'case.json',case)
    env=os.environ|dict(SST_LIB_PATH=build['plugin']+':'+build['library'],TILE_COMPONENT_OUTPUT=str(trial),PYTHONDONTWRITEBYTECODE='1')
    env.pop('TILE_COMPONENT_TRACE_START_TASK',None);env.pop('TILE_COMPONENT_PROGRAM_PROOF',None)
    with (trial/'simulation.log').open('w') as log:
        process=subprocess.Popen([build['sst'],'--num-threads=1',f'--output-json={trial/"topology.json"}',str(HERE/'simulation.py')],
            env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try:process.wait(timeout=180)
        finally:
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
            process.wait()
    assert process.returncode==0,(name,process.returncode,(trial/'simulation.log').read_text()[-6000:])
    return trial,case


def run(output,guest,variant,vlen,asq_depth,lsq_depth,bytecap,budget,model):
    name=f'{variant}-v{vlen}-a{asq_depth}-l{lsq_depth}-b{bytecap}-q{budget}'
    trial,case=simulate(output,name,guest,variant,vlen,asq_depth,lsq_depth,bytecap,budget,model)
    result=validate(trial,case)
    write(trial/'validation.json',dict(passed=True,validator_sha256=sha(Path(__file__)),
        queue_validator_sha256=sha(HERE/'validate.py'),result=result))
    return name,result


def compare_results(results):
    baseline=results.get('baseline-v256-a0-l16-b16384-q256');disabled=results.get('candidate-v256-a0-l16-b16384-q256')
    if baseline and disabled:
        assert baseline['memory_sha256']==disabled['memory_sha256'] and baseline['phases']==disabled['phases']
        assert baseline['cpu']['end_cycle']==disabled['cpu']['end_cycle']
    normal=results.get('candidate-v256-a4-l16-b16384-q256');replay=results.get('candidate-v256-a4-l16-b16384-q1')
    if normal and replay:
        assert normal['memory_sha256']==replay['memory_sha256'] and normal['phases']==replay['phases']
        assert normal['cpu']['end_cycle']==replay['cpu']['end_cycle']
        assert normal['canonical_trace_hashes']==replay['canonical_trace_hashes'],'Host budget changed modeled traces'
    return dict(disabled_matches_baseline=bool(baseline and disabled),budget_replay_checked=bool(normal and replay))


def main():
    parser=argparse.ArgumentParser(description=__doc__);add_model_arguments(parser)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--compile-only',action='store_true');parser.add_argument('--baseline-only',action='store_true')
    parser.add_argument('--candidate-only',action='store_true');parser.add_argument('--check-only',action='store_true')
    parser.add_argument('--asq-depth',type=int,choices=(0,1,4,8))
    args=parser.parse_args()
    if args.baseline_only and args.candidate_only:parser.error('Select at most one variant filter')
    if args.check_only and not args.output:parser.error('--check-only requires --output')
    if args.baseline_only and not args.baseline_build_info:parser.error('--baseline-only requires baseline model paths')
    output=(args.output or ROOT/'tests/results/source-new-analog-command-queue'/str(time.time_ns())).resolve()
    if output.exists() and any(output.iterdir()) and not args.check_only:parser.error('Output must be new or empty')
    output.mkdir(parents=True,exist_ok=True);print(output,flush=True)
    if args.check_only:
        cases=sorted(output.glob('*/case.json'))
        if not cases:parser.error('No saved cases found')
        results={}
        for path in cases:
            case=json.loads(path.read_text());assert sha(case['elf'])==case['sha256']
            results[case['name']]=validate(path.parent,case)
            write(path.parent/'validation.json',dict(passed=True,validator_sha256=sha(Path(__file__)),
                queue_validator_sha256=sha(HERE/'validate.py'),result=results[case['name']]))
            print('PASS',case['name'],flush=True)
        write(output/'validation.json',dict(passed=True,cases=len(results),failures=[],**compare_results(results),
            validator_sha256=sha(Path(__file__)),queue_validator_sha256=sha(HERE/'validate.py')))
        return
    guests={v:compile_guest(output/'guests'/f'v{v}',v,args.compiler) for v in (256,1024)}
    if args.compile_only:print('Compiled both guests');return
    models=models_from_arguments(args,parser,candidate=not args.baseline_only)
    variants=[('baseline',256,0,16,16384,256),('candidate',256,0,16,16384,256),
        ('candidate',256,1,16,16384,256),('candidate',256,4,16,16384,256),('candidate',256,8,16,16384,256),
        ('candidate',1024,4,16,16384,256),('candidate',1024,8,16,1024,256),
        ('candidate',256,4,1,16384,256),('candidate',256,4,16,16384,1)]
    variants=[v for v in variants if v[0] in models]
    if args.candidate_only:variants=[v for v in variants if v[0]=='candidate']
    if args.asq_depth is not None:variants=[v for v in variants if v[2]==args.asq_depth]
    if not variants:parser.error('Empty case selection')
    write(output/'metadata.json',dict(models=models,guests=guests,variants=variants,
        sources={p.name:sha(p) for p in HERE.iterdir() if p.is_file()}))
    results={};failures=[]
    for variant in variants:
        try:
            name,result=run(output,guests[variant[1]],*variant,models[variant[0]])
            results[name]=result;print('PASS',name,flush=True)
        except Exception as error:
            failures.append(dict(variant=variant,error=repr(error),traceback=traceback.format_exc()))
            print(traceback.format_exc(),flush=True)
    model_identities_unchanged(models)
    write(output/'validation.json',dict(passed=not failures,cases=len(results),requested_cases=len(variants),
        failures=failures,**compare_results(results),validator_sha256=sha(Path(__file__)),
        queue_validator_sha256=sha(HERE/'validate.py'),guest_source_sha256=sha(HERE/'directed.S')))
    raise SystemExit(bool(failures))


if __name__=='__main__':main()
