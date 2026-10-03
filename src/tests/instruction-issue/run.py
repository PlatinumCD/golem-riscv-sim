"""Check real issue timing, dependencies, unit throughput, and guest results."""
import argparse
from collections import Counter
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import load_build_info


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def cases():
    return [
        ('single', dict(issue_width=1)),
        ('dual', dict(issue_width=2)),
        ('dual-fetch-one', dict(issue_width=2, instruction_fetch_width=1)),
        ('dual-one-alu', dict(issue_width=2, integer_issue_units=1)),
        ('dual-two-memory', dict(issue_width=2, memory_issue_units=2)),
        ('alu-latency-three', dict(issue_width=2, integer_latency_cycles=3)),
        ('alu-interval-three', dict(issue_width=2, integer_latency_cycles=3, integer_initiation_interval=3)),
        ('vector-latency-three', dict(issue_width=2, vector_latency_cycles=3)),
        ('budget-one', dict(issue_width=2, instruction_budget=1)),
        ('budget-seven', dict(issue_width=2, instruction_budget=7)),
        ('uncached', dict(issue_width=2, instruction_cache_enabled=False)),
    ]


def compile_guest(output, compiler):
    support = SOURCE/'tests/riscv-qemu'
    elf = output/'guest.elf'
    cmd = [str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
           '-fuse-ld=lld', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0',
           '-nostdlib', '-static', '-Wl,--no-relax,--build-id=none', '-T', str(support/'scratchpad.ld'),
           str(support/'start.S'), str(HERE/'guest.S'),
           str(SOURCE/'tests/instruction-cache/measurement.S'), '-o', str(elf)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    (output/'guest.asm').write_text(subprocess.check_output([str(compiler.parent/'llvm-objdump'), '-d', str(elf)], text=True))
    symbols = {}
    for line in subprocess.check_output([str(compiler.parent/'llvm-nm'), '-n', str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3: symbols[fields[2]] = int(fields[0], 16)
    return elf, symbols, cmd


def validate(trial, options, symbols):
    log = (trial/'simulation.log').read_text()
    records = [json.loads(line.partition(' ')[2]) for line in log.splitlines() if line.startswith('RISCV_STATS ')]
    assert len(records) == 1
    cpu = records[0]
    assert cpu['memory_requests'] == cpu['completed_requests']
    data = (trial/'scratchpad.bin').read_bytes()[0x100000:0x1000a0]
    assert struct.unpack_from('<8Q', data) == (3, 7, 128, 64, 0, 10, 32, 17)
    assert struct.unpack_from('<4I', data, 128) == (3,)*4
    assert struct.unpack_from('<4I', data, 144) == (32,)*4
    issued = rows(trial/'riscv-issue.csv')
    width = options['issue_width']
    per_cycle = Counter(int(r['cycle']) for r in issued)
    assert max(per_cycle.values()) <= width
    assert len(issued) == cpu['issued_instructions'] == cpu['instructions']
    assert len(per_cycle) == cpu['issue_cycles']
    assert max(per_cycle.values()) == cpu['peak_issue_width']
    units = Counter((int(r['cycle']), r['unit']) for r in issued)
    for (cycle, unit), count in units.items():
        limit = options.get('integer_issue_units', 2) if unit == 'integer' else options.get('memory_issue_units', 1) if unit == 'memory' else 1
        assert count <= limit, (cycle, unit, count, limit)
    markers = rows(trial/'riscv-tasks.csv')
    assert len(markers) == 14
    phases = {}
    kernels = ('independent', 'dependent', 'mixed_rvv', 'pointer_chain', 'mixed_memory', 'multiply_chain', 'vector_chain')
    for name, start, end in zip(kernels, markers[::2], markers[1::2]):
        lo, hi = int(start['cycle']), int(end['cycle'])
        selected = [r for r in issued if lo <= int(r['cycle']) < hi and
                    symbols[name+'_begin'] <= int(r['pc']) < symbols[name+'_end']]
        cycles = [int(r['cycle']) for r in selected]
        assert cycles and cycles == sorted(cycles)
        phases[name] = dict(task_cycles=hi-lo, kernel_issue_span=cycles[-1]-cycles[0]+1,
                            instructions=len(cycles), peak_issue=max(Counter(cycles).values()))
        if name in ('dependent', 'multiply_chain', 'vector_chain'):
            latency = options.get('integer_latency_cycles', 1) if name == 'dependent' else 3 if name == 'multiply_chain' else options.get('vector_latency_cycles', 1)
            assert all(b-a >= latency for a,b in zip(cycles, cycles[1:])), (name, cycles, latency)
        if name == 'pointer_chain':
            accesses = [r for r in rows(trial/'riscv-memory.csv') if
                        lo <= int(r['cycle']) < hi and symbols[name+'_begin'] <= int(r['pc']) < symbols[name+'_end']]
            previous_ready = 0
            outstanding = False
            forwarded = 0
            for r in accesses:
                if r['event'] == 'issue':
                    assert not outstanding and int(r['cycle']) >= previous_ready
                    forwarded += int(r['cycle']) == previous_ready
                    outstanding = True
                elif r['event'] == 'ready':
                    assert outstanding
                    outstanding = False
                    previous_ready = int(r['cycle'])
            assert not outstanding
            if options.get('instruction_cache_enabled', True) and options.get('instruction_fetch_width', width) > 1:
                assert forwarded > 0, 'A dependency stall discarded every buffered next instruction'
    return dict(passed=True, cpu=cpu, phases=phases, result_sha256=hashlib.sha256(data).hexdigest())


def run(tools, output, name, options, qemu, elf, symbols):
    trial = output/name
    trial.mkdir()
    cpu = dict(load_store_queue_depth=8, scalar_load_store_queue_depth=8, analog_command_queue_depth=4)
    cpu.update(options)
    write(trial/'case.json', dict(elf=str(elf), qemu=str(qemu), cpu_parameters=cpu,
          parameters=dict(spm_capacity_bytes=2097152, spm_banks=8, spm_bank_width=4, riscv_vector_length_bits=128)))
    env = {k:v for k,v in os.environ.items() if not k.startswith(('TILE_COMPONENT_', 'TILE_CYCLE_PROFILE'))}
    env.update(SST_LIB_PATH=tools['plugin']+':'+tools['library'], TILE_COMPONENT_OUTPUT=str(trial),
               TILE_CYCLE_PROFILE='0', PYTHONDONTWRITEBYTECODE='1')
    with (trial/'simulation.log').open('w') as log:
        p = subprocess.Popen([tools['sst'], '--num-threads=1', str(HERE/'simulation.py')],
                             stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            p.wait(timeout=120)
        finally:
            if p.poll() is None: os.killpg(p.pid, signal.SIGKILL); p.wait()
    if p.returncode: raise RuntimeError(str(trial)+'\n'+(trial/'simulation.log').read_text()[-5000:])
    result = validate(trial, options, symbols)
    result.update(name=name, parameters=cpu)
    write(trial/'validation.json', result)
    print(f"PASS {name}: end={result['cpu']['end_cycle']}, independent={result['phases']['independent']['kernel_issue_span']}, dependent={result['phases']['dependent']['kernel_issue_span']}", flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--qemu', type=Path, default=ROOT/'build/src/qemu/qemu-system-riscv64')
    p.add_argument('--compiler', type=Path, default=ROOT/'install/llvm/bin/clang')
    p.add_argument('--build-info', type=Path, default=ROOT/'build/src/components/build.json')
    p.add_argument('--case', action='append')
    args = p.parse_args()
    selected = [(name, options) for name, options in cases() if not args.case or name in args.case]
    if args.case and set(args.case)-{name for name,_ in cases()}: p.error('Unknown case')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    tools = load_build_info(args.build_info)
    component = SOURCE/'components/riscv-qemu'
    native = output/'policy'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(component),
                    str(HERE/'policy.cc'), str(component/'instructionTiming.cc'), '-o', str(native)], check=True)
    subprocess.run([str(native)], check=True)
    elf, symbols, cmd = compile_guest(output, args.compiler.resolve())
    write(output/'metadata.json', dict(compiler_command=cmd, symbols=symbols, build_info=str(args.build_info.resolve()),
          qemu=str(args.qemu.resolve()), qemu_sha256=hashlib.sha256(args.qemu.read_bytes()).hexdigest()))
    results = [run(tools, output, name, options, args.qemu.resolve(), elf, symbols) for name,options in selected]
    assert len({r['result_sha256'] for r in results}) == 1
    by_name = {r['name']:r for r in results}
    if 'single' in by_name and 'dual' in by_name:
        a, b = by_name['single']['phases'], by_name['dual']['phases']
        assert a['independent']['kernel_issue_span'] == 128
        assert b['independent']['kernel_issue_span'] == 64
        assert a['dependent']['kernel_issue_span'] == b['dependent']['kernel_issue_span'] == 128
        assert b['mixed_rvv']['kernel_issue_span'] < a['mixed_rvv']['kernel_issue_span']
    for name in ('budget-one', 'budget-seven'):
        if name in by_name and 'dual' in by_name:
            assert by_name[name]['phases'] == by_name['dual']['phases']
            assert by_name[name]['cpu']['end_cycle'] == by_name['dual']['cpu']['end_cycle']
            for suffix in ('issue', 'issue-waits', 'memory', 'icache', 'slq', 'waits'):
                assert (output/name/f'riscv-{suffix}.csv').read_bytes() == (output/'dual'/f'riscv-{suffix}.csv').read_bytes(), (name,suffix)
    write(output/'results.json', results)
    write(output/'validation.json', dict(passed=True, cases=len(results)))
    print(f'PASS {len(results)} instruction issue cases: {output}', flush=True)


if __name__ == '__main__': main()
