#!/usr/bin/env python3
"""Run current-model regressions with fresh fixtures and isolated evidence."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from build import digest, load_module
from hardware_paths import ROOT, resolve_paths
from hardware_suite import selected_cases

FIXTURES = ('riscv-qemu/peer.cc', 'array-pipeline/protocol.cc', 'programming-delay/protocol.cc',
            'range-ordering/protocol.cc', 'bank-connections/driver.cc')


def execute(command, log, timeout, env):
    started = time.monotonic()
    record = dict(command=list(map(str, command)), log=str(log))
    with log.open('w') as stream:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
            record.update(exit_code=code, status='PASS' if code == 0 else 'FAIL')
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            record.update(exit_code=process.returncode, status='TIMEOUT')
    record['wall_seconds'] = time.monotonic() - started
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', default='hardware', choices=('hardware', 'compiler', 'all'))
    parser.add_argument('--case', action='append')
    parser.add_argument('--group')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--build-info', type=Path, help='Reuse a matching build containing all test fixtures')
    parser.add_argument('--qemu', type=Path)
    parser.add_argument('--profile', action='store_true', help='Collect cycle profiles in each simulation output/profiles directory')
    args = parser.parse_args()
    try:
        cases = selected_cases(args.suite, args.case, args.group)
    except ValueError as error:
        parser.error(str(error))
    if args.timeout < 1 or not cases:
        parser.error('Select at least one case and a positive timeout')
    if args.list:
        print('\n'.join(case.name for case in cases)); return
    paths = resolve_paths()
    build_root = Path(paths['GOLEM_BUILD_ROOT'])
    qemu = (args.qemu or build_root / 'qemu/qemu-system-riscv64').absolute()
    needs_runtime = any(case.qemu or case.name == 'platform/compressed-scalar' for case in cases)
    if needs_runtime and not qemu.is_file():
        parser.error('Build QEMU first: bash bootstrap.sh build-hardware')
    output = (args.output or ROOT / 'tests/results/hardware' / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f'Test evidence: {output}', flush=True)
    report = dict(schema='golem.component-regressions', status='RUNNING', source_tree=str(ROOT/'src'),
                  cases=[], qemu=str(qemu), git_head=subprocess.check_output(
                  ['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip())
    source_files = [path for folder in ('src', 'tools/hardware', 'tools/compiler/sculptor_deployment')
                    for path in (ROOT/folder).rglob('*')
                    if path.is_file() and path.suffix in ('.py', '.c', '.cc', '.h', '.S', '.ld', '.patch', '.inc')]
    source_files.append(ROOT / 'src/tile_profiles.json')
    report['source_sha256'] = {str(path): digest(path) for path in source_files}
    manifest = output / 'results.json'
    def save(): manifest.write_text(json.dumps(report, indent=2)+'\n')
    save()
    build_info = args.build_info
    component_build = load_module('regression_component_build', ROOT/'src/build.py')
    if any(case.build_info for case in cases):
        if build_info:
            component_build.load_build_info(build_info, with_tests=True,
                extra_sources=[ROOT/'src/tests'/name for name in FIXTURES])
        else:
            component_build.build(output/'build', with_tests=True,
                extra_sources=[ROOT/'src/tests'/name for name in FIXTURES])
            build_info = output/'build/build.json'
        report['build_info'] = str(build_info.resolve())
        report['build_info_sha256'] = digest(build_info)
    env = {k:v for k,v in os.environ.items()
           if not k.startswith(('TILE_COMPONENT_', 'TILE_CYCLE_PROFILE'))
           and k not in ('SST_LIB_PATH', 'TILE_PROFILE_SPM_CONNECTIONS')}
    env.pop('PYTHONOPTIMIZE', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['TILE_CYCLE_PROFILE'] = '1' if args.profile else '0'
    report['cycle_profiling'] = args.profile
    for case in cases:
        directory = output / case.name.replace('/', '-')
        script = case.script
        if case.name == 'analog/command-admission': script = script.with_name('admission.py')
        command = [sys.executable, '-B', str(script)]
        if case.name != 'host': command += ['--output', str(directory)]
        if case.build_info: command += ['--build-info', str(build_info)]
        if case.qemu: command += ['--qemu', str(qemu)]
        if case.name == 'platform/compressed-scalar': command += ['--after-qemu', str(qemu)]
        if case.name == 'network/mesh-2x2': command += ['--build-dir', str(output/'mesh-build')]
        command += list(case.arguments)
        print(f'RUN {case.name}', flush=True)
        record = execute(command, output/(case.name.replace('/','-')+'.log'), args.timeout, env)
        record['case'] = case.name
        report['cases'].append(record)
        save()
        print(f"{record['status']} {case.name} ({record['wall_seconds']:.1f}s)", flush=True)
        if record['status'] != 'PASS':
            print(Path(record['log']).read_text()[-5000:], flush=True)
    report['source_inputs_unchanged'] = all(path.is_file() and digest(path) == expected
        for name, expected in report['source_sha256'].items() for path in (Path(name),))
    report['status'] = 'PASS' if report['source_inputs_unchanged'] and all(c['status']=='PASS' for c in report['cases']) else 'FAIL'
    save()
    print(f"{report['status']}: {len(report['cases'])} suites; {manifest}", flush=True)
    if report['status'] != 'PASS': raise SystemExit(1)


if __name__ == '__main__': main()
