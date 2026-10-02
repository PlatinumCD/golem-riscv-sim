"""Verify initial whole-array delay, default compatibility, and real RVV timing."""
import argparse
from collections import Counter
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
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


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, label):
    matches = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + ' ')]
    assert len(matches) == 1, (label, matches)
    return matches[0]


def cases():
    result = []
    for pipeline in (False, True):
        for arrays in (1, 2):
            for scope in ('per_command', 'initial_full_array'):
                for delay in (0, 23):
                    result.append(dict(name=f'protocol-p{int(pipeline)}-a{arrays}-{scope}-d{delay}', kind='protocol',
                        parameters=resolve(dict(array_rows=2, array_cols=3, arrays_per_tile=arrays,
                            array_pipeline_enabled=pipeline, array_program_delay_scope=scope,
                            cost_per_array_program_cycles=delay, riscv_vector_length_bits=128))))
    for dimension, vlen in ((32, 256), (64, 512)):
        for lmul in (1, 2, 4, 8):
            for pipeline in (False, True):
                for scope, delay in (('per_command', 0), ('initial_full_array', 0), ('initial_full_array', 23)):
                    result.append(dict(name=f'rvv-n{dimension}-v{vlen}-m{lmul}-p{int(pipeline)}-{scope}-d{delay}',
                        kind='rvv', lmul=lmul, parameters=resolve(dict(array_rows=dimension, array_cols=dimension,
                            array_pipeline_enabled=pipeline, riscv_vector_length_bits=vlen, array_inflight_bytes=128,
                            array_program_delay_scope=scope, cost_per_array_program_cycles=delay))))
    result.append(dict(name='invalid-scope', kind='protocol', invalid_scope=True,
                       parameters=resolve(dict(array_rows=2, array_cols=3))))
    return result


def compile_guest(output, dimension, lmul):
    elf = output / f'guest-n{dimension}-m{lmul}.elf'
    support = HERE.parent / 'riscv-qemu'
    command = [str(ROOT / 'install/llvm/bin/clang'), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
        '-fuse-ld=lld', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0', '-O1',
        '-fno-vectorize', '-fno-slp-vectorize', '-ffreestanding', '-fno-builtin', '-nostdlib', '-static',
        '-Wl,--no-relax', '-Wl,--build-id=none', f'-DDIMENSION={dimension}', f'-DLMUL=m{lmul}',
        '-T', str(support / 'scratchpad.ld'), str(support / 'start.S'),
        str(HERE.parent / 'single-tile-runtime/measurement.S'), str(HERE / 'guest.c'), '-o', str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=60)
    elf.with_suffix('.build.log').write_text(process.stdout + process.stderr)
    assert process.returncode == 0, process.stderr
    return dict(elf=str(elf), sha256=sha256(elf), command=command)


def simulate(tools, trial, script):
    process = subprocess.run([tools['sst'], '--num-threads=1', f'--output-json={trial / "topology.json"}', str(script)],
        env=os.environ | dict(SST_LIB_PATH=tools['plugin'] + ':' + tools['library'],
            TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1'),
        capture_output=True, text=True, timeout=90)
    log = process.stdout + process.stderr
    (trial / 'simulation.log').write_text(log)
    return process.returncode, log


def validate(trial, case, log):
    p = case['parameters']
    initial = p['array_program_delay_scope'] == 'initial_full_array'
    delay = p['cost_per_array_program_cycles']
    array_stats = stats(log, 'ARRAY_STATS')
    trace = rows(trial / 'arrays.csv')
    events = rows(trial / 'array-programming.csv')
    complete = {int(row['token']): int(row['cycle']) for row in trace if row['event'] == 'complete'}
    last_service = {int(row['token']): int(row['cycle']) for row in trace if row['event'] == 'link_read'}
    total = p['array_rows'] * p['array_cols']
    coverage = [set() for _ in range(p['arrays_per_tile'])]
    delivered, starts, finishes = {}, {}, {}
    for event in events:
        token, cycle, array = (int(event[key]) for key in ('token', 'cycle', 'array'))
        assert event['scope'] == p['array_program_delay_scope']
        assert int(event['delay_cycles']) == delay and int(event['total_weights']) == total
        count, offset = int(event['element_count']), int(event['element_offset'])
        assert count > 0 and offset + count <= total
        if event['event'] == 'delivered':
            assert token not in delivered
            assert cycle == last_service[token] + 1, 'delivery must include final link transit'
            coverage[array].update(range(offset, offset + count))
            delivered[token] = event
        elif event['event'] == 'delay_start':
            assert token in delivered and token not in starts
            assert cycle == int(delivered[token]['cycle'])
            if initial: assert len(coverage[array]) == total
            starts[token] = cycle
        else:
            assert event['event'] == 'delay_complete' and token in starts and token not in finishes
            assert cycle == starts[token] + delay == complete[token]
            finishes[token] = cycle
        assert int(event['initialized_weights']) == len(coverage[array])
    assert starts.keys() == finishes.keys()
    assert all(len(covered) == total for covered in coverage)
    assert array_stats['program_delay_scope'] == p['array_program_delay_scope']
    assert array_stats['program_delay_charges'] == len(starts)
    assert array_stats['program_delay_cycles'] == delay * len(starts)
    assert array_stats['initial_full_array_completions'] == (p['arrays_per_tile'] if initial else 0)
    expected_charges = p['arrays_per_tile'] if initial else len(delivered)
    assert len(starts) == expected_charges
    for token, event in delivered.items():
        assert complete[token] == int(event['cycle']) + (delay if token in starts else 0)
    zeros = [row for row in trace if row['event'] == 'complete' and row['operation'] == '0' and row['element_count'] == '0']
    assert all(int(row['token']) not in delivered for row in zeros)
    if initial:
        by_array = {int(delivered[token]['array']): finish for token, finish in finishes.items()}
        for row in trace:
            if row['event'] == 'start' and row['operation'] == '2':
                assert int(row['cycle']) >= by_array[int(row['array'])], 'compute started before array readiness'
    result = dict(case=case['name'], arrays=array_stats, delay_charges=len(starts))
    if case['kind'] == 'protocol':
        report = stats(log, 'PROGRAMMING_PROTOCOL_RESULT')
        assert report['passed'] and report['arrays'] == p['arrays_per_tile']
        assert report['during_delay_rejections'] == (p['arrays_per_tile'] if initial and delay >= 16 else 0)
        assert array_stats['busy'] == 0 and array_stats['errors'] == report['errors']
        assert report['completed'] == array_stats['completed']
        assert len(delivered) == (4 if initial else 6) * p['arrays_per_tile']
        assert set(starts) == ({100 * a + 7 for a in range(p['arrays_per_tile'])} if initial else set(delivered))
        result['protocol'] = report
    else:
        assert array_stats['errors'] == array_stats['busy'] == 0 and array_stats['mvms'] == 1
        n = p['array_rows']
        assert len(delivered) == n * n // (case['lmul'] * p['riscv_vector_length_bits'] // 32)
        with (trial / 'scratchpad.bin').open('rb') as memory:
            memory.seek(0x100000)
            values = struct.unpack(f'<{n}f', memory.read(4 * n))
        expected = [sum(((r * n + c) % 11 - 5) * (c % 7 + 1) for c in range(n)) for r in range(n)]
        assert list(values) == expected
        markers = rows(trial / 'riscv-tasks.csv')
        assert len(markers) == 2 and [row['event'] for row in markers] == ['start', 'finish']
        result['program_cycles'] = int(markers[1]['cycle']) - int(markers[0]['cycle'])
        result['trace_sha256'] = sha256(trial / 'arrays.csv')
        result['cpu'] = stats(log, 'RISCV_STATS')
        result['program_chunks'] = len(delivered)
    return result


def compare(results, selected):
    by_name = {result['case']: result for result in results}
    checks = 0
    for case in selected:
        if case['kind'] != 'rvv' or case['parameters']['array_program_delay_scope'] != 'per_command': continue
        name = case['name']
        zero = name.replace('per_command-d0', 'initial_full_array-d0')
        delayed = name.replace('per_command-d0', 'initial_full_array-d23')
        if not {name, zero, delayed} <= by_name.keys(): continue
        old, base, added = (by_name[key] for key in (name, zero, delayed))
        assert old['program_cycles'] == base['program_cycles']
        assert old['trace_sha256'] == base['trace_sha256'], 'D=0 changed array event timing'
        assert old['cpu'] == base['cpu'], 'D=0 changed CPU timing/counters'
        # ASQ admission lets the CPU overlap its remaining loop/marker setup
        # with the final device delay. validate() checks the exact 23 cycles
        # from byte delivery to command completion; marker time need only
        # expose the unhidden part of that delay.
        assert 0 <= added['program_cycles'] - base['program_cycles'] <= 23
        assert old['program_chunks'] == base['program_chunks'] == added['program_chunks']
        checks += 1
    return checks


def legacy_regressions(output, tools):
    spec = importlib.util.spec_from_file_location('legacy_components', HERE.parent / 'run.py')
    legacy = importlib.util.module_from_spec(spec); spec.loader.exec_module(legacy)
    result = []
    for case in legacy.cases():
        if case.get('scenario', 'arrays') not in ('arrays', 'protocol', 'duplex'): continue
        trial = output / ('legacy-' + case['name']); trial.mkdir()
        (trial / 'case.json').write_text(json.dumps(case))
        (trial / 'parameters.json').write_text(json.dumps(resolve(case.get('parameters'))))
        code, log = simulate(tools, trial, HERE.parent / 'simulation.py')
        if case.get('expected_failure'):
            assert code and case['expected_failure'] in log
            result.append(dict(case=case['name'], expected_failure_verified=True))
        else:
            assert code == 0, log[-5000:]
            result.append(dict(case=case['name'], **legacy.validate(trial, case, log)))
        print('PASS legacy-' + case['name'], flush=True)
    legacy.compare(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--build-info', type=Path)
    parser.add_argument('--skip-legacy', action='store_true')
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case['name'] for case in selected}
        if unknown: parser.error(f'Unknown cases: {sorted(unknown)}')
        selected = [case for case in selected if case['name'] in args.case]
    output = (args.output or ROOT / 'tests/results/source-new-programming-delay' / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    if args.build_info:
        tools = json.loads(args.build_info.read_text())
        assert str(HERE / 'protocol.cc') in tools['command']
        for path, digest in tools['source_sha256'].items():
            assert sha256(path) == digest, f'Source changed: {path}'
    else:
        tools = build(ROOT / 'build/source_new-programming-delay', with_tests=True,
                      extra_sources=[HERE / 'protocol.cc', HERE.parent / 'array-pipeline/protocol.cc'])
    keys = sorted({(case['parameters']['array_rows'], case['lmul']) for case in selected if case['kind'] == 'rvv'})
    guests = {key: compile_guest(output, *key) for key in keys}
    (output / 'metadata.json').write_text(json.dumps(dict(build=tools, guests=list(guests.values())), indent=2) + '\n')
    results = []
    for case in selected:
        case['qemu'] = str(args.qemu.resolve())
        trial = output / case['name']; trial.mkdir()
        if case['kind'] == 'rvv': case['elf'] = guests[case['parameters']['array_rows'], case['lmul']]['elf']
        (trial / 'case.json').write_text(json.dumps(case, indent=2) + '\n')
        code, log = simulate(tools, trial, HERE / 'simulation.py')
        if case.get('invalid_scope'):
            assert code and 'array_program_delay_scope must be per_command or initial_full_array' in log
            results.append(dict(case=case['name'], expected_failure_verified=True))
        else:
            assert code == 0, log[-6000:]
            results.append(validate(trial, case, log))
        (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print('PASS ' + case['name'], flush=True)
    controls = compare(results, selected)
    legacy = [] if args.skip_legacy else legacy_regressions(output, tools)
    (output / 'legacy-results.json').write_text(json.dumps(legacy, indent=2) + '\n')
    report = dict(passed=True, cases=len(results), paired_controls=controls, legacy_cases=len(legacy))
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS ' + json.dumps(report), flush=True)


if __name__ == '__main__': main()
