"""Validate scalar queue hazards, overlap, fallback, and host-budget invariance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import load_build_info
from checks import check, records


def cases():
    return [dict(name='default', depth=8, options={})] + [
        dict(name=f'depth-{depth}', depth=depth,
             options=dict(scalar_load_store_queue_depth=depth))
        for depth in (0, 1, 2, 4, 8, 16, 64)
    ] + [
        dict(name='budget-1', depth=8, options=dict(instruction_budget=1)),
        dict(name='issue-4', depth=8, options=dict(issue_width=4)),
        dict(name='vector-depth-1', depth=8, options=dict(load_store_queue_depth=1)),
    ]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def compile_guest(output, compiler):
    support = SOURCE/'tests/riscv-qemu'
    elf = output/'scalar.elf'
    command = [str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
        '-fuse-ld=lld', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0',
        '-nostdlib', '-static', '-Wl,--no-relax,--build-id=none', '-T', str(support/'scratchpad.ld'),
        str(support/'start.S'), str(HERE/'guest.S'),
        str(SOURCE/'tests/array-pipeline/measurement.S'), '-o', str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    (output/'build.log').write_text(process.stdout+process.stderr)
    process.check_returncode()
    assembly = subprocess.check_output([str(compiler.parent/'llvm-objdump'), '-d', str(elf)], text=True)
    (output/'scalar.asm').write_text(assembly)
    return elf, assembly, command


def run(tools, output, case, qemu, elf, assembly):
    trial = output/case['name']
    trial.mkdir()
    cpu = dict(load_store_queue_depth=8, analog_command_queue_depth=4, instruction_budget=256)
    cpu.update(case['options'])
    write(trial/'case.json', dict(elf=str(elf), qemu=str(qemu), cpu_parameters=cpu,
        parameters=dict(spm_capacity_bytes=2*1024*1024, spm_banks=2, spm_bank_width=4,
                        riscv_vector_length_bits=128)))
    env = {k:v for k,v in os.environ.items()
           if not k.startswith(('TILE_COMPONENT_', 'TILE_CYCLE_PROFILE'))}
    env.update(SST_LIB_PATH=tools['plugin']+':'+tools['library'],
               TILE_COMPONENT_OUTPUT=str(trial), TILE_CYCLE_PROFILE='0', PYTHONDONTWRITEBYTECODE='1')
    with (trial/'simulation.log').open('w') as log:
        process = subprocess.Popen([tools['sst'], '--num-threads=1', str(HERE/'simulation.py')],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=env)
        try:
            process.wait(timeout=120)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    if process.returncode:
        raise RuntimeError(str(trial)+'\n'+(trial/'simulation.log').read_text()[-6000:])
    stats = check(trial, case['depth'], assembly)
    assert stats['scalar_load_store_queue_depth'] == case['depth']
    if case['depth'] == 0:
        assert stats['slq_enqueued'] == stats['slq_completed'] == stats['slq_stall_cycles'] == 0
        assert not records(trial/'riscv-slq.csv') and not records(trial/'riscv-slq-waits.csv')
    markers = records(trial/'riscv-tasks.csv')
    assert len(markers) == 6
    phases = {}
    for start,end in zip(markers[::2], markers[1::2]):
        assert start['event'] == 'start' and end['event'] == 'finish'
        assert start['task_id'] == end['task_id']
        phase = start['task_id']
        cycles = int(end['cycle'])-int(start['cycle'])
        field = 'write_bytes' if phase == '101' else 'read_bytes'
        size = int(end[field])-int(start[field])
        assert size == (512 if phase == '102' else 8192)
        phases[phase] = dict(cycles=cycles, bytes=size, bytes_per_cycle=size/cycles,
            scalar_stall_cycles=int(end['slq_stall_cycles'])-int(start['slq_stall_cycles']))
    result = dict(case=case['name'], passed=True, cpu=stats, phases=phases,
                  memory_sha256=digest(trial/'scratchpad.bin'))
    write(trial/'validation.json', result)
    print(f"PASS {case['name']}: {stats['end_cycle']} cycles, scalar peak {stats['slq_peak_occupancy']}", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--compiler', type=Path, default=ROOT/'install/llvm/bin/clang')
    parser.add_argument('--qemu', type=Path, default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--build-info', type=Path, default=ROOT/'build/src/components/build.json')
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case)-{c['name'] for c in selected}
        if unknown: parser.error(f'Unknown cases: {sorted(unknown)}')
        selected = [c for c in selected if c['name'] in args.case]
    tools = load_build_info(args.build_info)
    output = (args.output or ROOT/'tests/results/scalar-load-store-queue'/str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    elf, assembly, command = compile_guest(output, args.compiler.resolve())
    write(output/'metadata.json', dict(build_info=str(args.build_info.resolve()),
        qemu_sha256=digest(args.qemu.resolve()), elf_sha256=digest(elf), compiler_command=command,
        sources={str(p):digest(p) for p in HERE.iterdir() if p.is_file()}))
    results = [run(tools, output, c, args.qemu.resolve(), elf, assembly) for c in selected]
    assert len({r['memory_sha256'] for r in results}) == 1, 'Queue mode changed guest memory'
    by_name = {r['case']:r for r in results}
    if 'depth-8' in by_name and 'depth-0' in by_name:
        for phase in ('100', '101'):
            assert by_name['depth-8']['phases'][phase]['cycles'] < by_name['depth-0']['phases'][phase]['cycles']
    if 'depth-8' in by_name:
        for name in ('default', 'budget-1'):
            if name not in by_name: continue
            assert by_name[name]['phases'] == by_name['depth-8']['phases']
            assert by_name[name]['cpu']['end_cycle'] == by_name['depth-8']['cpu']['end_cycle']
            for path in (output/'depth-8').glob('*.csv'):
                assert path.read_bytes() == (output/name/path.name).read_bytes(), (name,path.name)
    write(output/'results.json', results)
    write(output/'validation.json', dict(passed=True, cases=len(results)))
    print(f'PASS {len(results)} scalar queue regressions: {output}', flush=True)


if __name__ == '__main__':
    main()
