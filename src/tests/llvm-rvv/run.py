#!/usr/bin/env python3
"""Fresh LLVM RVV correctness regressions, independent of study files."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = ROOT / 'src'
sys.path.insert(0, str(SOURCE))
from configuration import CPU_DEFAULTS, resolve
from build import load_build_info
from analysis import analyze_trial
from validate import APPLICATIONS, input_x, input_y, validate


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')



def compile_guest(output, compiler, application, implementation, vlen, elements):
    directory = output / 'guests' / f'{application}-{implementation}-vlen{vlen}'
    directory.mkdir(parents=True)
    blobs = {}
    for name, words, pattern in (('x', elements+2, input_x), ('y', elements, input_y)):
        path = directory / f'input-{name}.bin'
        path.write_bytes(struct.pack(f'<{words}I', *(pattern(index) for index in range(words))))
        blobs[name] = path
    data = directory / 'data.S'
    lines = []
    for name in ('x', 'y'):
        lines += [f'.section .rodata.input_{name},"a",@progbits', '.balign 64', '.fill 64,1,0xa5',
                  f'.incbin {json.dumps(str(blobs[name]))}', '.fill 64,1,0xa5']
    for name in ('output', 'warm_output'):
        lines += [f'.section .data.{name},"aw",@progbits', '.balign 64', '.fill 64,1,0xa5',
                  f'.zero {elements*4}', '.fill 64,1,0xa5']
    data.write_text('\n'.join(lines)+'\n')
    flags = [str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
        f'-march=rv64gcv_zvl{vlen}b_xgolemanalog',
        '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0', '-O3',
        '-ffreestanding', '-fno-builtin', f'-mrvv-vector-bits={vlen}',
        f'-DVECTOR_LENGTH_BITS={vlen}', f'-DELEMENT_COUNT={elements}',
        f'-DAPPLICATION={APPLICATIONS.index(application)}']
    commands, logs = [], []

    def execute(command):
        commands.append(command)
        process = subprocess.run(command, capture_output=True, text=True, timeout=120)
        logs.append(process.stdout+process.stderr)
        (directory / 'build.log').write_text('\n'.join(logs))
        if process.returncode:
            raise RuntimeError(f'Compilation failed: {directory}\n{process.stderr}')

    if implementation == 'llvm':
        kernel_source = HERE / 'kernel.c'
        execute(flags + ['-Rpass=loop-vectorize', '-Rpass-missed=loop-vectorize',
            '-Rpass-analysis=loop-vectorize', '-S', str(kernel_source), '-o', str(directory / 'kernel.s')])
    else:
        raise ValueError("This correctness fixture requires LLVM-generated kernels")
    execute(flags + ['-c', str(kernel_source), '-o', str(directory / 'kernel.o')])
    execute(flags + ['-c', str(HERE / 'main.c'), '-o', str(directory / 'main.o')])
    elf = directory / 'guest.elf'
    execute(flags + ['-fuse-ld=lld', '-nostdlib', '-static', '-Wl,--no-relax', '-Wl,--build-id=none',
        '-T', str(HERE / 'scratchpad.ld'), str(SOURCE / 'tests/riscv-qemu/start.S'),
        str(SOURCE / 'tests/single-tile-runtime/measurement.S'), str(HERE / 'measurement.S'),
        str(directory / 'kernel.o'), str(directory / 'main.o'), str(data), '-o', str(elf)])
    assembly = subprocess.check_output([str(compiler.parent / 'llvm-objdump'), '-d', str(elf)], text=True)
    (directory / 'guest.asm').write_text(assembly)
    kernel = (directory / 'kernel.s').read_text()
    vector = [line.split('#', 1)[0].strip() for line in kernel.splitlines() if re.match(r'^\s*v\w', line)]
    memory = [line for line in vector if re.match(r'v(?:l|s)(?:e\d+\.v|[1248]r(?:e\d+)?\.v)\s', line)]
    if not memory or 'mvm.' in kernel:
        raise RuntimeError(f'Expected ordinary RVV memory instructions in {directory}')
    if implementation == 'llvm' and 'vectorized loop' not in '\n'.join(logs):
        raise RuntimeError(f'LLVM did not report loop vectorization: {directory}')
    info = dict(application=application, implementation=implementation, vlen_bits=vlen, elements=elements,
        elf=str(elf), elf_sha256=sha256(elf), commands=commands,
        input_sha256={name: sha256(path) for name, path in blobs.items()},
        vector_instructions=vector, vector_memory_instructions=memory,
        vector_register_names=sorted(set(re.findall(r'\bv(?:[0-9]|[12][0-9]|3[01])\b', kernel))),
        vectorization_remarks='\n'.join(logs).strip())
    write_json(directory / 'compilation.json', info)
    return info


def run_case(output, case, build, timeout, analyze):
    trial = output / case['name']
    trial.mkdir()
    write_json(trial / 'case.json', case)
    write_json(trial / 'parameters.json', case['parameters'])
    command = [build['sst'], '--num-threads=1', f'--output-json={trial / "topology.json"}', str(HERE / 'simulation.py')]
    start = time.perf_counter()
    with (trial / 'simulation.log').open('w') as log:
        process = subprocess.Popen(command, env=os.environ | dict(SST_LIB_PATH=build['plugin']+':'+build['library'],
            TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1'), stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True)
        try:
            process.wait(timeout=timeout)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    log = (trial / 'simulation.log').read_text()
    if process.returncode:
        raise RuntimeError(f'SST failed ({process.returncode}): {log[-5000:]}')
    result = validate(trial, case, log)
    if analyze is not None:
        measurements = analyze(trial, Path(case['elf']), kernel_symbols=('transfer_kernel',))
        write_json(trial / 'measurements.json', measurements)
        result['measurements'] = measurements
        result.update(kernel_vector_density=measurements['kernel']['vector_density'],
            kernel_vector_memory_density=measurements['kernel']['vector_memory_density'],
            phase_vector_density=measurements['instructions']['vector_density'],
            phase_vector_memory_density=measurements['instructions']['vector_memory_density'],
            data_bytes_per_instruction=measurements['rates']['data_bytes_per_instruction'],
            lsq_mean_occupancy=measurements['lsq']['mean_occupancy'],
            async_vector_byte_fraction=measurements['lsq']['async_vector_byte_fraction'],
            scalar_read_bytes=measurements['traffic']['scalar_read_bytes'],
            scalar_write_bytes=measurements['traffic']['scalar_write_bytes'],
            vector_read_bytes=measurements['traffic']['vector_read_bytes'],
            vector_write_bytes=measurements['traffic']['vector_write_bytes'])
        result.update({f'cpu_{key}_cycles': value for key, value in measurements['cpu_cycles'].items()})
        result.update({f'backend_{key}': value for key, value in measurements['backend'].items()
                       if key.endswith('_utilization')})
    result['host_wall_seconds'] = time.perf_counter()-start
    write_json(trial / 'validation.json', dict(passed=True, result=result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--build-info', type=Path, default=ROOT / 'build/src/components/build.json')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'install/llvm/bin/clang')
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error('--jobs and --timeout must be positive')
    output = (args.output or ROOT / 'tests/results/llvm-rvv' / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = load_build_info(args.build_info)
    guests = {(app, vlen): compile_guest(output, args.compiler.resolve(), app, 'llvm', vlen, 256)
              for app in APPLICATIONS for vlen in (256, 1024)}
    cache = {key: value for key, value in CPU_DEFAULTS.items() if key.startswith('instruction_cache_')}
    cases = [dict(name=f'{app}-llvm-vlen{vlen}-banks8-depth{depth}', application=app, implementation='llvm',
        elements=256, vlen_bits=vlen, spm_banks=8, lsq_depth=depth, elf=guest['elf'],
        qemu=str(args.qemu.resolve()), parameters=resolve(dict(spm_capacity_bytes=32*1024*1024,
        spm_banks=8, riscv_vector_length_bits=vlen)),
        cpu_parameters=cache | dict(load_store_queue_depth=depth))
        for (app, vlen), guest in guests.items() for depth in (1, 4)]
    write_json(output / 'metadata.json', dict(cases=cases, guests=list(guests.values()), build=build,
        compiler_sha256=sha256(args.compiler), qemu_sha256=sha256(args.qemu)))
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_case, output, case, build, args.timeout, analyze_trial): case for case in cases}
        for future in as_completed(futures):
            case = futures[future]
            result = future.result()
            if case['application'] in ('copy', 'add') and case['lsq_depth'] == 4:
                assert result['measurements']['lsq']['enqueued'] > 0, 'LLVM transfers bypassed the LSQ'
            results.append(result)
            print(f"PASS {case['name']}", flush=True)
    write_json(output / 'validation.json', dict(passed=True, cases=len(results), guest_compilation=True,
        saved_artifacts_required=False, results=results))
    print(f"PASS {len(results)} LLVM RVV cases: {output}", flush=True)


if __name__ == '__main__':
    main()
