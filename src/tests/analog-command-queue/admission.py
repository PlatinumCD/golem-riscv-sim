#!/usr/bin/env python3
"""Exercise backend Busy retries and blocking Accepted-to-Error responses."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('asq_directed',HERE/'run.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)


def simulate(output,name,guest,variant,model):
    depth=0 if variant=='baseline' else 16
    trial,case=d.simulate(output,name,guest,variant,256,depth,16,16384,256,model)
    log=(trial/'simulation.log').read_text()
    cpu=d.existing.stats(log,'RISCV_STATS');arrays=d.existing.stats(log,'ARRAY_STATS')
    _,stalls=d.existing.check_lsq(trial,case,cpu)
    result=dict(cpu=cpu,arrays=arrays,lsq_stalls=stalls,memory_sha256=d.sha(trial/'scratchpad.bin'))
    result['asq']=d.validate_asq(trial,case,result)
    return trial,case,result


def save(trial,result):
    d.write(trial/'validation.json',dict(passed=True,validator_sha256=d.sha(Path(__file__)),
        queue_validator_sha256=d.sha(HERE/'validate.py'),result=result))


def main():
    parser=argparse.ArgumentParser(description=__doc__);d.add_model_arguments(parser)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    output=(args.output or d.ROOT/'tests/results/source-new-analog-command-admission'/str(time.time_ns())).resolve()
    if output.exists() and any(output.iterdir()):parser.error('Output must be new or empty')
    output.mkdir(parents=True,exist_ok=True);print(output,flush=True)
    models=d.models_from_arguments(args,parser)
    # Compile both guests from repository sources. No prior experiment or saved
    # run is needed; the first guest is the same sixteen-phase directed program.
    original=d.compile_guest(output/'guests/directed',256,args.compiler)
    guest=d.compile_guest(output/'guests/fallback',256,args.compiler,HERE/'admission.S')
    d.write(output/'metadata.json',dict(models=models,guests=dict(directed=original,fallback=guest),
        sources={p.name:d.sha(p) for p in HERE.iterdir() if p.is_file()}))
    trial,case,busy=simulate(output,'candidate-a16-busy',original,'candidate',models['candidate'])
    busy['outputs_checked']=d.expected_memory(trial,case)
    assert busy['cpu']['asq_busy']>0 and busy['asq']['peak_occupancy']==9,busy['asq']
    assert busy['asq']['events']['issue']==busy['asq']['events']['enqueue']+busy['asq']['events']['busy']
    save(trial,busy);print('PASS candidate-a16-busy',busy['cpu']['asq_busy'],flush=True)
    pair={};symbols=guest['symbols']
    for variant in ('baseline','candidate') if 'baseline' in models else ('candidate',):
        trial,case,result=simulate(output,variant+'-fallback',guest,variant,models[variant])
        memory=(trial/'scratchpad.bin').read_bytes();wanted=struct.pack('<64f',*range(1000,1064))
        assert memory[0x110000:0x110100]==wanted and memory[0x110100:0x111000]==bytes(3840)
        assert struct.unpack_from('<I',memory,0x1e0000)[0]==1
        assert struct.unpack_from('<QQ',memory,0x1e0008)==(2,symbols['target_fault'])
        assert result['arrays']['accepted']==2 and result['arrays']['completed']==result['cpu']['analog_commands']==1
        assert result['arrays']['errors']==1 and result['arrays']['mvms']==0
        if variant=='candidate':
            events=[d.numbers(row) for row in d.rows(trial/'riscv-asq.csv')]
            program=next(row['token'] for row in events if row['event']=='enqueue' and row['pc']==symbols['command_program'])
            response=next(row['token'] for row in events if row['event']=='enqueue' and row['pc']==symbols['target_fault'])
            first={row['event']:row['cycle'] for row in events if row['token']==program}
            second={row['event']:row['cycle'] for row in events if row['token']==response}
            assert 'guaranteed' in first and 'captured' in first and 'complete' in first
            assert 'accepted' in second and 'error' in second and 'guaranteed' not in second and 'complete' not in second
            fetches=[d.numbers(row) for row in d.rows(trial/'riscv-icache.csv')]
            handler=[row['cycle'] for row in fetches if row['event'] in ('hit','miss') and row['address']==symbols['trap_handler']]
            target=[row['cycle'] for row in fetches if row['event'] in ('hit','miss') and row['address']==symbols['target_fault']]
            assert len(handler)==len(target)==1
            assert first['captured']<=target[0]<first['complete']<=handler[0]
            assert second['accepted']<=second['error']<=handler[0]
            result['fallback_timing']=dict(program=first,output=second,fault_fetch=target[0],handler_fetch=handler[0])
        result['outputs_checked']=64;pair[variant]=result
        save(trial,result);print('PASS',variant+'-fallback',flush=True)
    if 'baseline' in pair:
        assert pair['baseline']['memory_sha256']==pair['candidate']['memory_sha256']
        for key in ('instructions','vector_instructions','read_bytes','write_bytes','analog_commands',
                    'analog_read_bytes','analog_write_bytes','vector_memory_beats'):
            assert pair['baseline']['cpu'][key]==pair['candidate']['cpu'][key],key
    d.model_identities_unchanged(models)
    d.write(output/'validation.json',dict(passed=True,cases=1+len(pair),
        backend_busy_responses=busy['cpu']['asq_busy'],busy_peak_occupancy=busy['asq']['peak_occupancy'],
        accepted_then_error_checked=True,precise_trap_after_captured_program=True,
        fallback_same_elf_memory_and_architectural_work='baseline' in pair,
        validator_sha256=d.sha(Path(__file__)),queue_validator_sha256=d.sha(HERE/'validate.py'),
        supplemental_guest_sha256=d.sha(HERE/'admission.S')))


if __name__=='__main__':main()
