#!/usr/bin/env python3
"""Real QEMU/SST scalar ALU overlap, retained hazards, traps and budget invariance."""
import argparse
import csv
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from configuration import resolve


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


lsq_checks = module('compressed_lsq_checks', SOURCE / 'tests/analog-register-dependencies/run.py')
asq_checks = module('compressed_asq_checks', SOURCE / 'tests/analog-command-queue/validate.py')
sha = lsq_checks.digest
dump = lsq_checks.dump

# Fixed operand/result oracles; each instruction executes with a pending RVV
# load, followed by a consumer that must still wait for the loaded registers.
SCALAR_CASES = [
    ('c.or a4,a5', 0x1234, 0xf0, 0x12f4),
    ('c.srli a4,33', -(1 << 63), 0, 1 << 30),
    ('c.srai a4,2', -8, 0, -2),
    ('c.andi a4,31', 0x1234, 0, 20),
    ('c.sub a4,a5', 8, 11, -3),
    ('c.xor a4,a5', 0xf0, 0xaa, 0x5a),
    ('c.and a4,a5', 0xf0, 0xaa, 0xa0),
    ('c.subw a4,a5', 0x80000000, 1, 0x7fffffff),
    ('divuw a4,a4,a5', 0x1234567880000001, 2, 0x40000000),
    ('remuw a4,a4,a5', 0x1234567880000001, 2, 1),
    ('mul a4,a4,a5', -13, 3, -39),
    ('divuw a4,a4,a5', 7, 0, -1),
    ('divw a4,a4,a5', -2147483648, -1, -2147483648),
    ('remw a4,a4,a5', -2147483648, -1, 0),
    ('mulw a4,a4,a5', 0x7fffffff, 2, -2),
    ('mulh a4,a4,a5', -(1 << 63), 2, -1),
    ('div a4,a4,a5', -9, 2, -4),
    ('divu a4,a4,a5', -(1 << 63), 2, 1 << 62),
    ('rem a4,a4,a5', -9, 2, -1),
    ('remu a4,a4,a5', -(1 << 63), 3, 2),
    ('mulhu a4,a4,a5', -1, 2, 1),
    ('mulhsu a4,a4,a5', -1, -1, -1),
]


def rows(path):
    with path.open() as stream:
        return [asq_checks.numbers(row) for row in csv.DictReader(stream)]


def compile_guest(output, compiler, fault_bytes=2):
    data = output / 'data.S'
    lines = ['.section .rodata,"a",@progbits']
    for name, values in [('weights', (float(i == j) for i in range(64) for j in range(64))),
                         ('input_data', range(1, 65)), ('noise_data', range(1000, 1064))]:
        words = [str(struct.unpack('<I', struct.pack('<f', value))[0]) for value in values]
        lines += ['.balign 4096', '.global ' + name, name + ':']
        lines += ['.word ' + ','.join(words[i:i+16]) for i in range(0, len(words), 16)]
    data.write_text('\n'.join(lines) + '\n')
    phases = []
    for phase, (instruction, lhs, rhs, _) in enumerate(SCALAR_CASES, 6):
        phases += [f'li a4,{lhs}', f'li a5,{rhs}', f'mark {phase},1',
                   '.option push', '.option rvc', '.balign 64', '.option pop',
                   f'.global memory_p{phase}', f'memory_p{phase}:', 'vle32.v v8,(s0)',
                   f'.global scalar_p{phase}', f'scalar_p{phase}:',
                   '.option push', '.option rvc', instruction, '.option pop',
                   f'.global dependent_p{phase}', f'dependent_p{phase}:',
                   'vadd.vi v16,v8,1', f'save 16,{phase}',
                   f'scalar {phase-2}', f'mark {phase},0']
    (output / 'scalar-phases.h').write_text('\n'.join(phases) + '\n')
    support = SOURCE / 'tests/riscv-qemu'
    elf = output / 'guest.elf'
    command = [str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
               '-fuse-ld=lld', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0',
               '-nostdlib', '-static', '-Wl,--no-relax', '-Wl,--build-id=none',
               f'-DFAULT_BYTES={fault_bytes}', '-I', str(output),
               '-T', str(support / 'scratchpad.ld'), str(support / 'start.S'),
               str(SOURCE / 'tests/array-pipeline/measurement.S'), str(HERE / 'directed.S'),
               str(data), '-o', str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    (output / 'build.log').write_text(process.stdout + process.stderr)
    assert process.returncode == 0, process.stderr
    (output / 'guest.asm').write_text(subprocess.check_output(
        [str(compiler.parent / 'llvm-objdump'), '-d', str(elf)], text=True))
    symbols = {}
    for line in subprocess.check_output(
            [str(compiler.parent / 'llvm-nm'), '--defined-only', str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    guest = dict(elf=str(elf), elf_sha256=sha(elf), symbols=symbols, command=command,
                 guest_source_sha256=sha(HERE / 'directed.S'), compiler_sha256=sha(compiler),
                 scalar_cases=SCALAR_CASES, fault_bytes=fault_bytes,
                 scalar_phases_sha256=sha(output / 'scalar-phases.h'))
    dump(output / 'guest.json', guest)
    return guest


def simulate(output, guest, build, variant, qemu, budget):
    trial = output / f'{variant}-budget{budget}'
    trial.mkdir()
    case = dict(name=trial.name, model=variant, variant=variant, vlen=256, depth=16,
                asq_depth=4, budget=budget, **guest, qemu=str(qemu), qemu_sha256=sha(qemu),
                component_source=str(SOURCE),
                parameters=resolve(dict(riscv_vector_length_bits=256, array_rows=64,
                    array_cols=64, spm_banks=1, array_pipeline_enabled=True,
                    cost_per_array_program_cycles=256, array_program_delay_scope='per_command',
                    array_inflight_bytes=128)),
                cpu_parameters=dict(load_store_queue_depth=16, instruction_budget=budget,
                    analog_command_queue_depth=4, analog_command_queue_bytes=1024,
                    host_timeout_seconds=120))
    dump(trial / 'case.json', case)
    environment = os.environ | dict(SST_LIB_PATH=build['plugin'] + ':' + build['library'],
                                   TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1')
    environment.pop('TILE_COMPONENT_TRACE_START_TASK', None)
    environment.pop('TILE_COMPONENT_PROGRAM_PROOF', None)
    with (trial / 'simulation.log').open('w') as log:
        process = subprocess.Popen([build['sst'], '--num-threads=1',
            f'--output-json={trial / "topology.json"}', str(HERE / 'simulation.py')],
            env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=180)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    log = (trial / 'simulation.log').read_text()
    assert process.returncode == 0, (case['name'], log[-10000:])
    return trial, case, log


def validate(trial, case, log):
    cpu = lsq_checks.stats(log, 'RISCV_STATS')
    arrays = lsq_checks.stats(log, 'ARRAY_STATS')
    memory = (trial / 'scratchpad.bin').read_bytes()
    words = lambda values: b''.join(struct.pack('<f', value) for value in values)
    incremented = b''.join(struct.pack('<I', struct.unpack('<I', struct.pack('<f', value))[0] + 1)
                           for value in range(1, 65))
    expected = {0: incremented, 1: words(range(1000, 1064)), 2: words(range(1000, 1064)),
                3: bytes(256), 4: incremented, 5: words(range(1000, 1064))}
    expected.update({phase: incremented for phase in range(6, 6+len(SCALAR_CASES))})
    for slot, wanted in expected.items():
        offset = 0x110000 + 4096 * slot
        assert memory[offset:offset+256] == wanted, ('Vector output', case['name'], slot)
        assert memory[offset+256:offset+4096] == bytes(4096-256), ('Unexpected output tail', slot)
    scalars = struct.unpack_from(f'<{4+len(SCALAR_CASES)}q', memory, 0x1d0000)
    assert scalars == (-2147483648, 2, -1, 2147483647, *(entry[3] for entry in SCALAR_CASES)), (
        'Scalar arithmetic', scalars)
    count, cause, pc = struct.unpack_from('<QQQ', memory, 0x1e0000)
    assert (count, cause, pc) == (1, 2, case['symbols']['fault_p5']), ('Trap', count, cause, pc)
    assert memory[0x1e0100:0x1e0200] == words(range(1, 65)), 'Trap vector state'
    assert memory[0x1e0018:0x1e001c] == words([1000]), 'Trap older store visibility'
    entries, stalls = lsq_checks.check_lsq(trial, case, cpu)
    assert cpu['memory_requests'] == cpu['completed_requests']
    assert arrays['accepted'] == arrays['completed'] == cpu['analog_commands']
    assert arrays['errors'] == 0 and arrays['mvms'] == 1
    result = dict(cpu=cpu, arrays=arrays, lsq_stalls=stalls)
    result['asq'] = asq_checks.validate_asq(trial, case, result)
    markers = rows(trial / 'riscv-tasks.csv')
    fetches = rows(trial / 'riscv-icache.csv')
    asq = rows(trial / 'riscv-asq.csv')
    lsq = rows(trial / 'riscv-lsq.csv')
    asq_waits = rows(trial / 'riscv-asq-waits.csv')
    assert len(markers) == 2*(5+len(SCALAR_CASES))
    phases = {}
    for phase in range(1, 6+len(SCALAR_CASES)):
        pair = [row for row in markers if row['task_id'] == phase]
        assert [row['event'] for row in pair] == ['start', 'finish']
        lo, hi = [row['cycle'] for row in pair]
        info = phases[str(phase)] = dict(start=lo, end=hi, cycles=hi-lo)

        def fetch(symbol):
            cycles = [row['cycle'] for row in fetches if row['event'] in ('hit', 'miss')
                      and row['address'] == case['symbols'][symbol] and lo <= row['cycle'] <= hi]
            assert len(cycles) == 1, (symbol, cycles)
            return cycles[0]

        def issued(symbol):
            selected = [entry for entry in entries if entry['pc'] == case['symbols'][symbol]]
            assert len(selected) == 8, (symbol, selected)
            return selected

        if phase != 5:
            scalar = info['scalar_fetch'] = fetch(f'scalar_p{phase}')
            target = info['dependent_fetch'] = fetch(f'dependent_p{phase}')
        if phase in (1, 2) or phase >= 6:
            older = issued(f'memory_p{phase}')
            complete = info['older_memory_complete'] = max(entry['complete'] for entry in older)
            assert min(entry['enqueue'] for entry in older) <= scalar
            assert (scalar >= complete if case['variant'] == 'before' else scalar < complete), info
            if phase != 2:
                assert complete <= target, ('Vector RAW was lost', info)
            else:
                younger = issued('dependent_p2')
                for new in younger:
                    old = next(entry for entry in older if entry['address'] == new['address'])
                    assert old['service_complete'] <= new['service_complete'], ('Memory order', old, new)
                info['overlapping_read_last_complete'] = max(entry['complete'] for entry in younger)
        if phase in (3, 4, 5):
            command = [row for row in asq if row['event'] == 'enqueue'
                       and row['pc'] == case['symbols'][f'command_p{phase}']]
            assert len(command) == 1
            events = {row['event']: row for row in asq
                      if row['token'] == command[0]['token'] and row['event'] != 'stall'}
            complete = info['command_complete'] = events['complete']['cycle']
            if phase == 3:
                capture = info['source_capture'] = events['captured']['cycle']
                assert capture <= target, ('Array source WAR was lost', info)
                assert (scalar >= complete if case['variant'] == 'before' else scalar < capture), info
                if case['variant'] == 'after':
                    assert target < complete, ('Source pinned through full programming', info)
            elif phase == 4:
                assert complete <= target, ('Array output RAW was lost', info)
                assert (scalar >= complete if case['variant'] == 'before' else scalar < complete), info
            else:
                handler = info['trap_handler_fetch'] = fetch('trap_handler')
                assert complete <= handler, ('Trap before array retirement', info)
                older = issued('memory_p5_load') + issued('memory_p5_store')
                info['older_memory_complete'] = max(entry['complete'] for entry in older)
                assert info['older_memory_complete'] <= handler, ('Trap before memory retirement', info)
                # The LSQ stall trace has no PC. This phase contains exactly one
                # drain while the directed memory and array work are both live.
                drains = [row for row in lsq if row['event'] == 'stall' and row['reason'] == 'drain'
                          and max(entry['enqueue'] for entry in older) <= row['cycle']
                          < min(entry['complete'] for entry in older)]
                assert len(drains) == 1 and drains[0]['occupancy'] == 16, ('Trap LSQ pressure', drains)
                info['trap_lsq_drain_cycle'] = drains[0]['cycle']
                assert command[0]['cycle'] <= drains[0]['cycle'] < complete
                waits = [row for row in asq_waits if row['pc'] == case['symbols']['fault_p5']]
                assert len(waits) == 1 and waits[0]['reason'] == 'drain', ('Trap ASQ wait', waits)
                assert waits[0]['start_cycle'] == info['older_memory_complete']
                assert waits[0]['end_cycle'] == complete <= fetch('fault_p5') <= handler
    result.update(phases=phases, scalar_results=scalars, trap_checked=True,
                  memory_sha256=sha(trial / 'scratchpad.bin'),
                  canonical_trace_hashes=lsq_checks.canonical_trace_hashes(trial))
    dump(trial / 'validation.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-info', type=Path, default=ROOT / 'build/source_new-mordred-posted/build.json')
    parser.add_argument('--before-qemu', type=Path, default=ROOT / 'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--after-qemu', type=Path, required=True,
                        help='Current source_new QEMU build with scalar arithmetic queue overlap')
    parser.add_argument('--variants', nargs='+', choices=['before', 'after'], default=['before', 'after'])
    parser.add_argument('--budgets', nargs='+', type=int, default=[1, 256])
    parser.add_argument('--compiler', type=Path, default=ROOT / 'install/llvm/bin/clang')
    parser.add_argument('--fault-bytes', type=int, choices=[2, 4], default=2,
                        help='Reserved compressed (2) or M word (4) instruction')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True)
    build = json.loads(args.build_info.read_text())
    plugin = Path(build['plugin']) / 'libtilecomponents.so'
    models = {name: getattr(args, name + '_qemu').resolve() for name in args.variants}
    identities = {str(path): sha(path) for path in [args.build_info.resolve(), plugin, *models.values()]}
    guest = compile_guest(output, args.compiler.resolve(), args.fault_bytes)
    results = {}
    for variant, qemu in models.items():
        for budget in args.budgets:
            trial, case, log = simulate(output, guest, build, variant, qemu, budget)
            results[trial.name] = validate(trial, case, log)
            print(f'{trial.name}: all {5+len(SCALAR_CASES)} directed phases passed', flush=True)
    for variant in models:
        paired = [results[f'{variant}-budget{budget}'] for budget in args.budgets]
        assert all(item['phases'] == paired[0]['phases'] for item in paired)
        assert all(item['memory_sha256'] == paired[0]['memory_sha256'] for item in paired)
        # Shared ASQ and array token identities can depend on host grant count.
        # Compare all CPU/SPM CSVs exactly; phase and lifetime assertions audit ASQ.
        stable = lambda item: {name: value for name, value in item['canonical_trace_hashes'].items()
                              if not name.startswith('array') and name != 'riscv-asq.csv'}
        assert all(stable(item) == stable(paired[0]) for item in paired), ('Budget altered trace', variant)
    assert all(sha(path) == expected for path, expected in identities.items()), 'Executable changed during tests'
    assert len({item['memory_sha256'] for item in results.values()}) == 1, 'Architectural memory changed'
    summary = dict(passed=True, cases=len(results), phases_per_case=5+len(SCALAR_CASES), identities=identities,
                   guest=guest, results=results)
    dump(output / 'validation.json', summary)
    print(json.dumps(dict(passed=True, cases=len(results), output=str(output))))


if __name__ == '__main__':
    main()
