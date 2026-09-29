"""Compile tensor models with Sculptor and execute one tile on source_new."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import build
from configuration import resolve
from models import CASES, fixture

LLVM = ROOT/'install/llvm/bin'
OPT = ROOT/'install/sculptor-mlir/bin/sculptor-mlir-opt'


def command(args, log, commands):
    args = list(map(str, args))
    commands.append(args)
    result = subprocess.run(args, capture_output=True, text=True, timeout=180)
    with log.open('a') as stream:
        stream.write(json.dumps(args)+'\n'+result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(f'Command failed; see {log}\n{result.stderr[-5000:]}')
    return result.stdout


def compile_model(out, model, epochs):
    out.mkdir(parents=True, exist_ok=True)
    commands=[]; log=out/'build.log'
    (out/'source.mlir').write_text(model['source'])
    (out/'inputs.json').write_text(json.dumps(model['inputs']))
    (out/'expected.json').write_text(json.dumps(model['expected']))
    rows, cols = model['array_rows'], model['array_cols']
    flags=['--sculptor-canonicalize-layers','--sculptor-convert-layers','--canonicalize','--cse',
           f'--sculptor-expand-mvm-to-golem=array-rows={rows} array-cols={cols}',
           '--canonicalize','--cse','--sculptor-tag-layers','--sculptor-construct-ra-tree',
           '--sculptor-ra-order-setups','--sculptor-ra-add-iterations','--sculptor-ra-add-spatial-cuts',
           '--sculptor-ra-plan-execution-groups',
           f'--sculptor-ra-init-resources=mesh-rows=1 mesh-cols=1 arrays-per-tile=4 array-rows={rows} array-cols={cols} spm-bytes-per-tile=2097152',
           '--sculptor-ra-distribute-resources=policy=packed',
           '--sculptor-ra-assign-operation-ownership','--sculptor-ra-plan-value-sharing',
           '--sculptor-ra-assign-value-ownership','--sculptor-build-tile-programs',
           '--sculptor-lower-tile-programs=vectorize=true','--sculptor-ra-plan-buffers',
           '--sculptor-ra-plan-transfers',
           f'--sculptor-place-logical-tiles=algorithm=snake output-file={out/"plan.json"} program-file={out/"program.mlir"}']
    command([OPT,out/'source.mlir',*flags,'-o',out/'expanded.mlir'],log,commands)
    plan=json.loads((out/'plan.json').read_text())['tile_program_plan']
    if len(plan['tiles'])!=1 or plan['tiles'][0]['id']!=0:
        raise ValueError('source_new runner requires exactly Logical Tile 0')
    if len(plan['tiles'][0]['arrays']) != model['arrays']:
        raise ValueError('unexpected array allocation')
    command([OPT,out/'program.mlir',
             f'--sculptor-materialize-tile-deployment=logical-tile=0 epochs={epochs} input-file={out/"inputs.json"} output-file={out/"deployment.json"} shared-ram-image={out/"shared-ram.bin"}',
             '-o',out/'resolved.mlir'],log,commands)
    deployment=json.loads((out/'deployment.json').read_text())
    if any(c['source_tile']>=0 and c['destination_tile']>=0 for c in deployment['connections']):
        raise ValueError('source_new local runner cannot execute inter-tile connections')
    command([OPT,out/'resolved.mlir','--sculptor-deployment-to-llvm','-o',out/'llvm.mlir'],log,commands)
    command([LLVM/'mlir-translate','--mlir-to-llvmir',out/'llvm.mlir','-o',out/'tile.ll'],log,commands)
    payload_base=deployment['shared_ram_image']['global_offset']
    end=max(c['global_offset']+(epochs-1)*c['epoch_bytes']+c['bytes'] for c in deployment['connections'])
    payload=bytearray((out/'shared-ram.bin').read_bytes())
    if end-payload_base > 2097152:
        raise ValueError('single-tile external payload exceeds local scratchpad')
    payload.extend(bytes(end-payload_base-len(payload)))
    (out/'payload.bin').write_bytes(payload)
    (out/'payload.S').write_text(f'''.section .data,"aw",@progbits
.balign 64
.globl sculptor_single_tile_guard_before
sculptor_single_tile_guard_before:
.fill 64,1,0x5a
.globl sculptor_single_tile_payload
sculptor_single_tile_payload:
.incbin {json.dumps(str(out/'payload.bin'))}
.globl sculptor_single_tile_payload_end
sculptor_single_tile_payload_end:
.globl sculptor_single_tile_guard_after
sculptor_single_tile_guard_after:
.fill 64,1,0x5a
.section .rodata
.balign 8
.globl sculptor_single_tile_payload_global_offset
sculptor_single_tile_payload_global_offset:
.quad {payload_base}
''')
    tile=deployment['tiles'][0]
    flags=['--target=riscv64-unknown-elf','-mcpu=golem-analog','-mabi=lp64d','-mcmodel=medany',
           '-msmall-data-limit=0','-O2','-ffp-contract=off','-ffreestanding','-fno-builtin',
           '-fno-stack-protector','-ffunction-sections','-fdata-sections']
    objects=[]
    for name,path in [('body',out/'tile.ll'),('payload',out/'payload.S'),
                      ('start',SOURCE/'tests/riscv-qemu/start.S'),
                      ('runtime',ROOT/'tools/compiler/sculptor_deployment/single_tile_runtime.cc')]:
        obj=out/f'{name}.o'; objects.append(obj)
        command([LLVM/('clang++' if name=='runtime' else 'clang'),*flags,
                 *(['-std=c++17','-fno-exceptions','-fno-rtti'] if name=='runtime' else []),
                 '-c',path,'-o',obj],log,commands)
    # Sigmoid's expf remains a normal library operation, rebuilt for SPM addresses.
    if 'math.exp' in model['source']:
        libm=ROOT/'third_party/riscv-gnu-toolchain/newlib/newlib/libm'
        for name in ('sf_exp','sf_exp2','sf_exp2_data','math_errf'):
            obj=out/f'{name}.o'; objects.append(obj)
            command([LLVM/'clang',*flags,'-D_IEEE_LIBM',f'-I{libm/"common"}',
                     '-c',libm/'common'/f'{name}.c','-o',obj],log,commands)
    command([LLVM/'clang++',*flags,'-nostdlib','-static','-fuse-ld=lld','-Wl,--no-relax',
             '-Wl,--gc-sections','-Wl,--build-id=none',
             f'-Wl,--defsym,SPM_CODE_OFFSET={tile["code_offset"]},--defsym,SPM_BYTES=2097152,--defsym,STACK_BYTES={tile["stack_bytes"]}',
             '-Wl,--defsym,__sculptor_spm_begin=0x90000000,--defsym,__sculptor_spm_end=0x90200000',
             '-T',ROOT/'src/platform/startup/scratchpad.ld',*objects,'-o',out/'tile.elf'],log,commands)
    assembly=command([LLVM/'llvm-objdump','-d',out/'tile.elf'],log,commands)
    (out/'tile.asm').write_text(assembly)
    assert all(op in assembly for op in ('mvm.vset','mvm.vl','mvm.vs','vsetvli'))
    assert not re.search(r'\bmvm\.(?:set|l|s|mv)\s',assembly)
    symbols={}
    for line in command([LLVM/'llvm-nm','--defined-only',out/'tile.elf'],log,commands).splitlines():
        fields=line.split()
        if len(fields)==3: symbols[fields[2]]=int(fields[0],16)
    (out/'symbols.json').write_text(json.dumps(symbols,indent=2)+'\n')
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    return deployment,symbols


def run_case(out, model, deployment, symbols, vlen, tools, qemu, pipeline, scope):
    trial=out/f'vlen{vlen}'; trial.mkdir()
    parameters=resolve(dict(array_rows=model['array_rows'],array_cols=model['array_cols'],
                            arrays_per_tile=4,riscv_vector_length_bits=vlen,
                            array_pipeline_enabled=pipeline,array_program_delay_scope=scope,
                            cost_per_array_program_cycles=7))
    config=dict(parameters=parameters,elf=str(out/'tile.elf'),qemu=str(qemu))
    (trial/'case.json').write_text(json.dumps(config,indent=2)+'\n')
    args=[tools['sst'],'--num-threads=1',f'--output-json={trial/"topology.json"}',str(HERE/'simulation.py')]
    start=time.monotonic()
    with (trial/'simulation.log').open('w') as log:
        p=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT,start_new_session=True,
                           env=os.environ|{'SST_LIB_PATH':tools['plugin']+':'+tools['library'],
                                           'TILE_COMPONENT_OUTPUT':str(trial),'PYTHONDONTWRITEBYTECODE':'1'})
        try: p.wait(timeout=120)
        finally:
            try: os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError: pass
            p.wait()
    log=(trial/'simulation.log').read_text()
    if p.returncode: raise RuntimeError(f'{trial}:\n{log[-6000:]}')
    stats={k:next(json.loads(line[len(k)+1:]) for line in log.splitlines() if line.startswith(k+' '))
           for k in ('RISCV_STATS','SPM_STATS','ARRAY_STATS')}
    cpu,spm,arrays=(stats[k] for k in ('RISCV_STATS','SPM_STATS','ARRAY_STATS'))
    assert cpu['memory_requests']==cpu['completed_requests']==spm['completed']==spm['accepted']
    assert arrays['errors']==arrays['busy']==0
    epochs=len(model['inputs']); mvms=model['mvms_per_input']*epochs
    assert arrays['mvms']==mvms
    weight_bytes=model['arrays']*model['array_rows']*model['array_cols']*4
    assert cpu['analog_write_bytes']==weight_bytes+mvms*model['array_cols']*4
    assert cpu['analog_read_bytes']==mvms*model['array_rows']*4
    if scope=='initial_full_array': assert arrays['initial_full_array_completions']==model['arrays']
    memory=(trial/'scratchpad.bin').read_bytes()
    def read_symbol(name,size):
        offset=symbols[name]-0x90000000
        return memory[offset:offset+size]
    for name in ('sculptor_single_tile_guard_before','sculptor_single_tile_guard_after'):
        assert read_symbol(name,64)==b'\x5a'*64
    base=deployment['shared_ram_image']['global_offset']
    address=symbols['sculptor_single_tile_payload']-0x90000000
    original=(out/'payload.bin').read_bytes()
    for segment in deployment['shared_ram_image']['segments']:
        offset=segment['global_offset']-base; size=segment['bytes']
        assert memory[address+offset:address+offset+size]==original[offset:offset+size]
    outputs=[c for c in deployment['connections'] if c['destination_tile']<0]
    assert len(outputs)==1
    c=outputs[0]; actual=[]
    for epoch in range(epochs):
        offset=address+c['global_offset']-base+epoch*c['epoch_bytes']
        actual.append(list(struct.unpack('<'+str(c['bytes']//4)+'f',memory[offset:offset+c['bytes']])))
    errors=[]
    for got,wanted in zip(actual,model['expected']):
        assert len(got)==len(wanted)
        for a,b in zip(got,wanted):
            assert math.isfinite(a) and abs(a-b)<=2e-6+2e-6*abs(b),(a,b)
            errors.append(abs(a-b))
    result=dict(passed=True,vlen=vlen,epochs=epochs,arrays=model['arrays'],array_pipeline=pipeline,
                program_delay_scope=scope,maximum_absolute_error=max(errors),
                simulated_cycles=cpu['end_cycle'],host_seconds=time.monotonic()-start,
                weights_programmed_bytes=weight_bytes,outputs=actual,stats=stats)
    (trial/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'PASS {out.name} VLEN{vlen}: {epochs} inputs, {model["arrays"]} arrays, {mvms} MVMs, max error {max(errors):.3g}',flush=True)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',action='append',choices=CASES)
    parser.add_argument('--vlen',action='append',type=int,choices=(256,512,1024))
    parser.add_argument('--epochs',type=int,default=3)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--build-info',type=Path)
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--array-pipeline',action='store_true')
    parser.add_argument('--program-delay-scope',choices=('per_command','initial_full_array'),default='per_command')
    args=parser.parse_args()
    if args.epochs<2: parser.error('at least two inputs are required to test resident weights')
    output=(args.output or ROOT/'tests/results/source-new-sculptor'/str(time.time_ns())).resolve()
    output.mkdir(parents=True,exist_ok=False)
    if args.build_info:
        tools=json.loads(args.build_info.read_text())
        for name,digest in tools['source_sha256'].items():
            if hashlib.sha256(Path(name).read_bytes()).hexdigest()!=digest:
                raise RuntimeError(f'Build input changed: {name}; rebuild with source_new/build.py')
    else: tools=build(output/'build')
    results=[]
    for case in args.case or CASES:
        model=fixture(case,args.epochs); out=output/case
        deployment,symbols=compile_model(out,model,args.epochs)
        for vlen in args.vlen or (256,512):
            results.append(dict(case=case,**run_case(out,model,deployment,symbols,vlen,tools,args.qemu.resolve(),args.array_pipeline,args.program_delay_scope)))
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'PASS {len(results)} Sculptor / source_new end-to-end cases: {output}',flush=True)


if __name__=='__main__': main()
