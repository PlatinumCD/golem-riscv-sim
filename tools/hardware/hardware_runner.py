#!/usr/bin/env python3
"""Hardware-first test execution with isolated artifacts and independent result gates."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from build import digest, link_dependency, hardware_inputs, input_identity
from hardware_paths import ROOT, resolve_paths
from hardware_suite import GROUPS, selected_cases

sys.path.insert(0, str(ROOT / 'src/sst/tests'))
from check_measurements import check_roots, ValidationError

BINARY_NAMES = ('qemu/bin/qemu-system-riscv64', 'sst-elements/lib/sst-elements-library/libmittens.so')


def save(path, value):
    temporary = path.with_suffix('.json.new')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def execute(command, env, log, timeout):
    started = time.monotonic()
    result = {'command': list(map(str, command)), 'log': str(log), 'exit_code': None}
    with log.open('w') as stream:
        try:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                result['exit_code'] = process.wait(timeout=timeout)
                result['status'] = 'PASS' if result['exit_code'] == 0 else 'FAIL'
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                result['status'] = 'TIMEOUT'
        except OSError as error:
            result.update(status='FAIL', error=str(error))
            stream.write(str(error) + '\n')
    result['wall_seconds'] = time.monotonic() - started
    return result


def measurements(directory, required=False, identity=None):
    summaries = list(directory.rglob('tile-*-summary.json'))
    legacy = list(directory.rglob('tile-*-summary.csv'))
    errors = [f'missing JSON companion: {path}' for path in legacy if not path.with_suffix('.json').is_file()]
    if not summaries:
        return {'status': 'FAIL' if required or errors else 'NOT_APPLICABLE', 'summaries': 0,
                'errors': errors or (['required simulator summaries missing'] if required else [])}
    try:
        count, invalid = check_roots([directory], identity)
        errors.extend(invalid)
    except (OSError, ValueError, ValidationError) as error:
        count = len(summaries)
        errors.append(str(error))
    return {'status': 'FAIL' if errors else 'PASS', 'summaries': count, 'errors': errors}


def counters(directory):
    # Preserve duplicate metric names and row order, rather than collapsing to a dict.
    result = {}
    for path in sorted(directory.rglob('tile-*-summary.csv')):
        with path.open(newline='') as stream:
            result[str(path.relative_to(directory))] = list(csv.reader(stream))
    return result


def source_fingerprints():
    return {str(path.relative_to(ROOT)): digest(path)
            for folder in ('src', 'tools/hardware', 'build-scripts', 'tests/support')
            for path in sorted((ROOT / folder).rglob('*'))
            if path.is_file() and '__pycache__' not in path.parts}


def run_cases(cases, hardware, output, timeout, *, historical_baseline=False):
    output.mkdir(parents=True, exist_ok=False)
    report = {'schema': 'golem.hardware-tests', 'schema_version': 1, 'status': 'RUNNING',
              'output': str(output), 'hardware_installation': str(hardware), 'timeout_seconds': timeout,
              'measurement_policy': 'historical_baseline_not_validated' if historical_baseline else 'current_strict',
              'cases': [], 'source_sha256': source_fingerprints(),
              'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'git_status': subprocess.check_output(['git', 'status', '--porcelain=v1'], cwd=ROOT, text=True),
              'git_index_sha256': digest(ROOT / '.git/index')}
    save(output / 'results.json', report)
    print(f'Test evidence: {output}', flush=True)
    inherited = {key: value for key, value in os.environ.items()
                 if not key.startswith(('MITTENS_', 'GOLEM_')) and key not in ('SST_LIB_PATH', 'QEMU_SYSTEM_RISCV64')}
    install = output / 'install'
    env = dict(inherited, GOLEM_HARDWARE_TREE='src', GOLEM_BUILD_ROOT=str(output / 'setup'),
               GOLEM_INSTALL_ROOT=str(install), GOLEM_LLVM_DIR=str(ROOT / 'install/llvm'),
               GOLEM_SOURCE_ROOT=str(output / 'sources'), PYTHONDONTWRITEBYTECODE='1',
               SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'),
               MITTENS_TEST_QEMU=str(install / 'qemu/bin/qemu-system-riscv64'),
               QEMU_SYSTEM_RISCV64=str(install / 'qemu/bin/qemu-system-riscv64'))
    report['environment'] = {key: value for key, value in env.items() if key.startswith(('GOLEM_', 'SST_', 'MITTENS_', 'QEMU_'))}
    report['discarded_fixture_keys'] = sorted(key for key in os.environ if key.startswith('MITTENS_'))
    try:
        report['binary_sha256'] = {name: digest(hardware / name) for name in BINARY_NAMES}
        for dependency in ('sst-core', 'llvm', 'cross-sim'):
            link_dependency(ROOT / 'install' / dependency, install / dependency)
        for dependency in ('qemu', 'sst-elements'):
            link_dependency(hardware / dependency, install / dependency)
        dependencies = [ROOT / 'install' / name for name in
                        ('sst-core/bin/sst', 'sst-core/etc/sst/sstsimulator.conf', 'llvm/bin/clang++',
                         'sst-elements/lib/sst-elements-library/libmerlin.so',
                         'sst-elements/lib/sst-elements-library/libmemHierarchy.so')]
        report['dependency_sha256'] = {str(path.resolve()): digest(path) for path in dependencies}
        report['preflight'] = {'status': 'PASS'}
    except (OSError, RuntimeError) as error:
        report.update(status='FAIL', preflight={'status': 'FAIL', 'error': str(error)})
        report['cases'] = [{'case': case.name, 'status': 'SKIP', 'reason': 'required dependency preflight failed'} for case in cases]
        save(output / 'results.json', report)
        return report

    report['provenance'] = execute(['python3', '-B', str(ROOT / 'tools/hardware/check-build.py')],
                                   env, output / 'provenance.log', timeout)
    report['runtime_build'] = execute(['bash', str(ROOT / 'build-scripts/build-runtime.sh')],
                                      env, output / 'runtime-build.log', timeout)
    save(output / 'results.json', report)
    identity = None
    if report['provenance']['status'] == 'PASS':
        provenance = sorted((output / 'setup/checks').glob('*/provenance.json'))
        if provenance:
            identity = json.loads(provenance[-1].read_text()).get('implementation_id')
            report['provenance_artifact'] = str(provenance[-1])
        if not historical_baseline:
            report['provenance']['measurements'] = measurements(output / 'setup/checks', True, identity)
            if report['provenance']['measurements']['status'] != 'PASS' or not identity:
                report['provenance']['status'] = 'FAIL'
            report['expected_implementation_id'] = input_identity(hardware_inputs())
            if identity != report['expected_implementation_id']:
                report['provenance'].update(status='FAIL', error='Loaded Mittens does not match current implementation inputs; rebuild hardware')
    for case in cases:
        trial = output / 'cases' / case.name
        trial.mkdir(parents=True)
        build = trial / 'artifacts'
        build.mkdir()
        # Profile owners already emit configuration beside each profile. A single
        # case-wide override would overwrite tile-0.json across internal sweeps.
        case_env = dict(env, GOLEM_BUILD_ROOT=str(build), GOLEM_TEST_CASE_NAME=case.name)
        entry = execute(case.command(), case_env, trial / 'execution.log', timeout)
        entry['case'] = case.name
        entry['execution_status'] = entry['status']
        entry['measurements'] = ({'status': 'NOT_VALIDATED', 'reason': 'explicit historical binary baseline'}
                                 if historical_baseline else measurements(build, case.measurements_required, identity))
        if entry['status'] == 'PASS' and entry['measurements']['status'] == 'FAIL':
            entry['status'] = 'FAIL'
        entry['guest_sha256'] = {str(path.relative_to(build)): digest(path) for path in sorted(build.rglob('*.elf'))}
        entry['resolved_configurations'] = [str(path) for path in sorted(trial.rglob('*.json'))
                                             if path.parent.name in ('resolved', 'resolved-configurations')]
        report['cases'].append(entry)
        save(output / 'results.json', report)
        print(f'{case.name}: {entry["status"]} ({entry["wall_seconds"]:.1f}s; measurements={entry["measurements"]["status"]})', flush=True)
    report['binary_sha256_after'] = {name: digest(hardware / name) for name in BINARY_NAMES}
    report['binaries_unchanged'] = report['binary_sha256'] == report['binary_sha256_after']
    report['dependencies_unchanged'] = all(digest(Path(path)) == expected for path, expected in report['dependency_sha256'].items())
    report['git_index_unchanged'] = digest(ROOT / '.git/index') == report['git_index_sha256']
    report['status'] = ('PASS' if all(entry['status'] == 'PASS' for entry in report['cases'])
                        and all(report[key]['status'] == 'PASS' for key in ('provenance', 'runtime_build'))
                        and report['binaries_unchanged'] and report['dependencies_unchanged'] else 'FAIL')
    save(output / 'results.json', report)
    print(f'{report["status"]}: {len(cases)} cases; {output / "results.json"}', flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--suite', choices=('hardware', 'compiler', 'models', 'all'), default='hardware')
    selection.add_argument('--group', choices=GROUPS)
    parser.add_argument('--case', action='append')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    try:
        cases = selected_cases(args.suite, args.case, args.group)
        paths = resolve_paths()
    except ValueError as error:
        parser.error(str(error))
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    if args.list:
        print('\n'.join(case.name for case in cases))
        return 0
    report = run_cases(cases, Path(paths['GOLEM_INSTALL_ROOT']),
                       Path(__file__).resolve().parents[2] / 'tests/results/test-runs' / str(time.time_ns()), args.timeout)
    return int(report['status'] != 'PASS')


if __name__ == '__main__':
    raise SystemExit(main())
