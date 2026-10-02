"""Check masked analog state, finite queues, physical stride, tails and actual byte counts."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import build, load_build_info
from configuration import resolve


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def records(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, prefix):
    result = [json.loads(line[len(prefix)+1:]) for line in log.splitlines() if line.startswith(prefix+' ')]
    assert len(result) == 1, (prefix,result)
    return result[0]


def matrix_hash(matrix):
    result = 14695981039346656037
    for byte in struct.pack('<'+'f'*len(matrix), *matrix):
        result = ((result ^ byte) * 1099511628211) & ((1 << 64)-1)
    return result


def compile_guest(output, lmul):
    elf = output / f'guest-{lmul}.elf'
    support = HERE.parent / 'riscv-qemu'
    command = [str(ROOT/'install/llvm/bin/clang'), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
        '-fuse-ld=lld', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0', '-O1',
        '-fno-vectorize', '-fno-slp-vectorize', '-ffreestanding', '-fno-builtin', '-nostdlib', '-static',
        '-Wl,--no-relax', '-Wl,--build-id=none', f'-DLMUL={lmul}',
        '-T', str(support/'scratchpad.ld'), str(support/'start.S'), str(HERE/'guest.c'), '-o', str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=60)
    elf.with_suffix('.build.log').write_text(process.stdout+process.stderr)
    assert process.returncode == 0, process.stderr
    return dict(elf=str(elf), sha256=sha(elf), command=command)


def validate(trial, case, log):
    array = stats(log, 'ARRAY_STATS')
    assert array['busy'] == 0
    commands = records(trial/'arrays.csv')
    read_bytes = sum(int(r['bytes']) for r in commands if r['event']=='link_read')
    write_bytes = sum(int(r['bytes']) for r in commands if r['event']=='link_write')
    assert read_bytes == array['link_read_bytes'] and write_bytes == array['link_write_bytes']
    proof = records(trial/'array-initial-program.csv')
    p = case['parameters']
    if case['kind']=='protocol':
        result = stats(log,'ACTIVE_REGION_RESULT')
        assert result['passed'] and result['expected_errors'] == array['errors']
        matrices = [[10.0]*20, [1,2,3,0,0,4,5,6,0,0]+[0.0]*10, [-2]+[0.0]*19]
        expected_shapes = [(4,5),(2,3),(1,1)]
        delivered = [80,36,4]
        assert array['mvms'] == (5 if p['array_pipeline_enabled'] else 4)
        assert read_bytes == 20*4+5*4+9*4+3*4+3*4+1*4+1*4
        assert write_bytes == (4+1+1+1+2+1+(2 if p['array_pipeline_enabled'] else 0))*4
        # A queued third computation cannot start until both active output
        # rows retire, even when row 0 is read twice.
        if p['array_pipeline_enabled']:
            executes = [r for r in commands if r['event']=='start' and r['operation']=='2']
            stores = [r for r in commands if r['event']=='complete' and r['operation']=='3' and r['element_offset']=='1']
            assert int(executes[3]['cycle']) >= int(stores[0]['cycle'])
    else:
        cpu = stats(log,'RISCV_STATS')
        assert cpu['memory_requests'] == cpu['completed_requests']
        expected_shapes = [(17,19),(3,5),(1,1),(17,19)]
        matrices = []
        for rows, cols in expected_shapes:
            matrix = [0.0]*1024
            for r in range(rows):
                for c in range(cols): matrix[r*32+c] = (r*19+c)%7-3
            matrices.append(matrix)
        delivered = [r*c*4 for r,c in expected_shapes]
        invocations = [(17,19,0),(17,19,1),(3,5,2),(1,1,3),(17,19,4)]
        expected = [sum(((r*19+c)%7-3)*(c%5+1+phase) for c in range(cols))
                    for rows,cols,phase in invocations for r in range(rows)]
        with (trial/'scratchpad.bin').open('rb') as memory:
            memory.seek(0x100000)
            actual = list(struct.unpack('<'+'f'*len(expected),memory.read(4*len(expected))))
        assert actual == expected, (actual,expected)
        assert read_bytes == sum(delivered) + sum(c*4 for _,c,_ in invocations)
        assert write_bytes == sum(r*4 for r,_,_ in invocations)
        assert array['mvms'] == 5
        result = dict(cpu=cpu, numerical_results=len(actual), tails_preserved=True)
    if p['array_program_delay_scope']=='initial_full_array':
        assert len(proof)==len(expected_shapes), proof
        for epoch,(item,(r,c),matrix,byte_count) in enumerate(zip(proof,expected_shapes,matrices,delivered),1):
            assert (int(item['active_rows']),int(item['active_cols']))==(r,c)
            assert int(item['active_weights'])==int(item['initialized_weights'])==r*c
            assert int(item['total_weights'])==p['array_rows']*p['array_cols']
            assert int(item['delivered_bytes'])==byte_count
            assert int(item['weights_fnv1a64'])==matrix_hash(matrix)
            assert int(item['configuration_epoch'])==epoch
        assert array['initial_full_array_completions']==len(proof)
        assert array['program_delay_charges']==len(proof)
    else:
        assert not proof
    return dict(result=result, arrays=array, proof_epochs=len(proof), valid_only_bytes_verified=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-info',type=Path)
    parser.add_argument('--qemu',type=Path,required=True)
    parser.add_argument('--kind',choices=('protocol','rvv'))
    args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    tools=load_build_info(args.build_info,extra_sources=[HERE/'protocol.cc']) if args.build_info else build(
        output/'components',extra_sources=[HERE/'protocol.cc'])
    cases=[]
    for pipeline in (False,True):
        for scope in ('per_command','initial_full_array'):
            cases.append(dict(name=f'protocol-p{int(pipeline)}-{scope}',kind='protocol',parameters=resolve(dict(
                array_rows=4,array_cols=5,arrays_per_tile=1,array_pipeline_enabled=pipeline,
                cost_per_mvm_cycles=100,cost_per_array_program_cycles=23,array_program_delay_scope=scope,
                riscv_vector_length_bits=128,array_inflight_bytes=5))))
    for vlen in (128,256,512):
        for pipeline in (False,True):
            for queue in (0,4):
                cases.append(dict(name=f'rvv-v{vlen}-p{int(pipeline)}-q{queue}',kind='rvv',lmul='m8',
                    parameters=resolve(dict(array_rows=32,array_cols=32,arrays_per_tile=1,
                        array_pipeline_enabled=pipeline,cost_per_mvm_cycles=100,cost_per_array_program_cycles=23,
                        array_program_delay_scope='initial_full_array',riscv_vector_length_bits=vlen)),
                    cpu_parameters=dict(load_store_queue_depth=16,scalar_load_store_queue_depth=8,
                                        analog_command_queue_depth=queue,host_timeout_seconds=60)))
    # Small register groups ensure valid rows/columns span multiple VL chunks.
    cases.append(dict(cases[-1], name='rvv-v128-m1-p1-q4', lmul='m1',
                      parameters=resolve(dict(cases[-1]['parameters'],riscv_vector_length_bits=128,array_link_width=16))))
    if args.kind: cases=[case for case in cases if case['kind']==args.kind]
    guests={lmul:compile_guest(output,lmul) for lmul in {c['lmul'] for c in cases if c['kind']=='rvv'}}
    (output/'metadata.json').write_text(json.dumps(dict(build=tools,qemu=str(args.qemu.resolve()),
        qemu_sha256=sha(args.qemu),guests=guests),indent=2)+'\n')
    results=[]
    for case in cases:
        case['qemu']=str(args.qemu.resolve())
        if case['kind']=='rvv':case['elf']=guests[case['lmul']]['elf']
        trial=output/case['name'];trial.mkdir()
        (trial/'case.json').write_text(json.dumps(case,indent=2)+'\n')
        env=os.environ|dict(SST_LIB_PATH=tools['plugin']+':'+tools['library'],TILE_COMPONENT_OUTPUT=str(trial),
            TILE_COMPONENT_PROGRAM_PROOF='1',PYTHONDONTWRITEBYTECODE='1')
        process=subprocess.run([tools['sst'],'--num-threads=1',f'--output-json={trial/"topology.json"}',
            str(HERE/'simulation.py')],env=env,capture_output=True,text=True,timeout=120)
        log=process.stdout+process.stderr;(trial/'simulation.log').write_text(log)
        assert process.returncode==0,(case['name'],log[-6000:])
        results.append(dict(case=case['name'],**validate(trial,case,log)))
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('PASS '+case['name'],flush=True)
    (output/'validation.json').write_text(json.dumps(dict(passed=True,cases=len(results)),indent=2)+'\n')


if __name__=='__main__': main()
