#!/usr/bin/env python3
"""Run isolated hardware guests, preserving commands, hashes and partial failures."""
import argparse
import hashlib
import os
import signal
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tools/hardware'))
from hardware_paths import resolve_paths, require_owned_output
from cases import registry, REGRESSION
from contract import save


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def output_directory(requested, paths, environment=None):
    env = os.environ if environment is None else environment
    # The suite supplies a unique per-case artifact owner via GOLEM_BUILD_ROOT.
    owner = (Path(paths['GOLEM_BUILD_ROOT']) if env.get('GOLEM_BUILD_ROOT')
             else ROOT / 'tests/results')
    output = requested or owner / 'communication-envelope' / str(time.time_ns())
    require_owned_output(output, owner)
    return output.resolve()


def sources():
    return {str(p.relative_to(ROOT)): digest(p)
            for base in (HERE, ROOT/'src/sst', ROOT/'src/platform', ROOT/'tests/support')
            for p in base.rglob('*') if p.is_file() and '__pycache__' not in p.parts}


def header(case):
    mapping = {'PayloadBytes': case.payload, 'Waves': case.waves, 'FlowCount': len(case.flows),
               'TileCount': case.width*case.height, 'RVVTile': case.rvv_tile,
               'RVVIterations': case.rvv_iterations, 'DelayTile': case.delay_tile,
               'DelayInstructions': case.delay_instructions, 'GapInstructions': case.gap_instructions,
               'BankPhase': int(case.bank_phase)}
    text = '#pragma once\n#include <stdint.h>\n'
    for key, value in mapping.items():
        text += f'constexpr {"int" if key in ("RVVTile", "DelayTile") else "uint32_t"} {key} = {value};\n'
    for key, index in (('Sources', 0), ('Destinations', 1)):
        values = ','.join(str(pair[index]) for pair in case.flows) or '0'
        text += f'constexpr uint32_t {key}[] = {{{values}}};\n'
    return text


def execute(command, env, log, timeout):
    start = time.monotonic()
    with log.open('w') as output:
        try:
            process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                       cwd=ROOT, start_new_session=True)
            process.wait(timeout=timeout)
            status = 'PASS' if process.returncode == 0 else 'FAIL'
            code = process.returncode
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            status, code = 'TIMEOUT', None
        except OSError as error:
            output.write(f'Launch failed: {error}\n')
            status, code = 'FAIL', None
    return dict(command=command, status=status, exit_code=code,
                wall_seconds=round(time.monotonic()-start, 3), log=str(log))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append')
    parser.add_argument('--category', action='append')
    parser.add_argument('--regression', action='store_true')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--hardware-install', type=Path, help='Pinned QEMU/Mittens for a diagnostic comparison; SST Core remains shared')
    args = parser.parse_args()
    cases = registry()
    names = args.case or (list(REGRESSION) if args.regression else list(cases))
    if any(n not in cases for n in names):
        parser.error('select known cases')
    if args.category:
        names = [n for n in names if cases[n].study in args.category]
    if not names or any(n not in cases for n in names):
        parser.error('select known, nonempty cases')
    if args.list:
        for n in names: print(f'{cases[n].study:22} {n}')
        return 0
    paths = resolve_paths()
    install = Path(paths['GOLEM_INSTALL_ROOT'])
    output = output_directory(args.output, paths)
    output.mkdir(parents=True, exist_ok=False)
    hardware = args.hardware_install or install
    binaries = {'qemu':hardware/'qemu/bin/qemu-system-riscv64',
                'sst':install/'sst-core/bin/sst',
                'mittens':hardware/'sst-elements/lib/sst-elements-library/libmittens.so'}
    source_hashes = sources()
    report = dict(schema='golem.communication-envelope', schema_version=2, status='RUNNING',
                  selected=names, cases=[], source_sha256=source_hashes,
                  binary_sha256={n:digest(p) for n,p in binaries.items()},
                  scope='hardware only; no deployment runtime or compiler/model workloads',
                  frozen_machine=dict(spm_bytes=524288, banks=8, spm_bits=256, link_bits=32,
                    clock='1GHz', packet_words=64, routing='XY', memory_backend='streaming'))
    save(output/'run.json', report)
    print(f'Evidence: {output}', flush=True)
    env = {k:v for k,v in os.environ.items() if not k.startswith(('MITTENS_', 'GOLEM_', 'ENVELOPE_'))}
    env.update(paths, PYTHONDONTWRITEBYTECODE='1', MITTENS_TEST_QEMU=str(binaries['qemu']),
               SST_LIB_PATH=str(hardware/'sst-elements/lib/sst-elements-library'))
    for name in names:
        case = cases[name]; trial = output/name; trial.mkdir()
        for folder in ('profile','tasks','serial'): (trial/folder).mkdir()
        save(trial/'case.json', case.document())
        (trial/'config.h').write_text(header(case))
        trial_env = dict(env, ENVELOPE_TRIAL=str(trial))
        entry = dict(name=name, study=case.study, status='RUNNING')
        report['cases'].append(entry); save(output/'run.json',report)
        entry['build'] = execute(['make','-f',str(HERE/'guest/Makefile'),
            f'OUTPUT_DIR={trial/"elf"}', f'CONFIG={trial/"config.h"}',
            'TILES='+' '.join(map(str,case.document()['active_tiles']))], trial_env,trial/'build.log',60)
        if entry['build']['status'] != 'PASS':
            entry['status'] = 'BUILD_FAILED'
        else:
            entry['guest_sha256'] = {p.name:digest(p) for p in (trial/'elf').glob('*.elf')}
            entry['simulation'] = execute([str(binaries['sst']),str(HERE/'simulation.py')],
                                         trial_env,trial/'simulation.log',args.timeout)
            entry['status'] = entry['simulation']['status']
            if entry['status'] == 'PASS':
                entry['analysis'] = execute([sys.executable,'-B',str(HERE/'analyze.py'),str(trial)],
                                            trial_env,trial/'analysis.log',30)
                entry['status'] = entry['analysis']['status']
        save(output/'run.json',report)
        print(f'{name}: {entry["status"]}', flush=True)
    report['binaries_unchanged'] = report['binary_sha256'] == {n:digest(p) for n,p in binaries.items()}
    report['sources_unchanged'] = sources() == source_hashes
    report['status'] = 'PASS' if all(c['status']=='PASS' for c in report['cases']) and report['binaries_unchanged'] and report['sources_unchanged'] else 'FAIL'
    save(output/'run.json',report)
    print(f'{report["status"]}: {output/"run.json"}',flush=True)
    return int(report['status'] != 'PASS')


if __name__ == '__main__':
    raise SystemExit(main())
