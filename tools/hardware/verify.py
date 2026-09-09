#!/usr/bin/env python3
"""Run current host component checks; historical source comparison is opt-in."""
import hashlib
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

TOOLS = Path(__file__).resolve().parent
COPY = TOOLS.parents[1] / 'src'


def without_local_includes(text):
    return re.sub(r'^#include "[^"]+"$', '#include "LOCAL"', text, flags=re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-copy-equivalence', action='store_true',
                        help='Also require the pre-refactor include-only equivalence')
    parser.add_argument('--reference-source', type=Path,
                        help='Original-layout source snapshot, required only for copy equivalence')
    args = parser.parse_args()
    if args.check_copy_equivalence:
        if args.reference_source is None or not args.reference_source.is_dir():
            parser.error('--check-copy-equivalence requires an existing --reference-source directory')
        entries = json.loads((COPY.parent / 'docs/hardware/history/source-path-map.json').read_text())
        for entry in entries:
            original, cleaned = args.reference_source / entry['original'], COPY / entry['cleaned']
            if not original.is_file() or not cleaned.is_file():
                parser.error(f'incompatible reference snapshot: {original} / {cleaned}')
            assert hashlib.sha256(original.read_bytes()).hexdigest() == entry['sha256'], original
            if cleaned.suffix in ('.cc', '.cpp', '.c', '.h', '.inc', '.S', '.ld', '.patch', '.env'):
                assert without_local_includes(original.read_text()) == without_local_includes(cleaned.read_text()), cleaned
        print(f'{len(entries)} historical copy entries verified.', flush=True)
    elif args.reference_source:
        parser.error('--reference-source requires --check-copy-equivalence')
    cases = {
        'cpu_execution_ledger': [],
        'cpu_execution_controller': ['execution/cpuExecutionController.cc',
                                     'execution/qemuCaptureCoordinator.cc',
                                     'execution/qemuReadySetExecutor.cc',
                                     'bridge/sharedSyncMemoryBridge.cc'],
        'memory_address_map': [],
        'clock_domain': [],
        'receive_dma_engine': ['network/receiveDMAEngine.cc'],
        'injection_order': [],
        'scratchpad_timing_model': ['memory/scratchpad/scratchpadTimingModel.cc'],
        'scratchpad_observer': ['memory/scratchpad/scratchpadTimingModel.cc'],
        'memory_access_coalescer': [],
        'global_ram_readiness': [],
        'task_trace': ['profiling/taskTrace.cc'],
        'tile_progress': ['profiling/tileProgress.cc', 'profiling/performanceProfile.cc',
                          'profiling/measurementWriter.cc'],
        'performance_profile': ['profiling/performanceProfile.cc',
                                'profiling/measurementWriter.cc'],
        'measurement_writer': ['profiling/performanceProfile.cc',
                               'profiling/measurementWriter.cc'],
    }
    output = Path(tempfile.mkdtemp(prefix='golem-src-check-'))
    for name, sources in cases.items():
        executable = output / name
        test = COPY / 'sst/tests' / (name + '_test.cpp')
        if name == 'measurement_writer':
            test = COPY / 'sst/profiling/tests/measurement_writer_test.cpp'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(COPY / 'bridge/include'), str(test)]
        if name == 'measurement_writer':
            command += ['-I' + str(COPY.parent / 'third_party/sst-core/external')]
        command.extend(str(COPY / 'sst' / source) for source in sources)
        if name == 'cpu_execution_controller':
            command.append('-pthread')
        subprocess.run(command + ['-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        if name == 'cpu_execution_controller':
            for mode in ('runtime-cancel', 'runtime-success', 'late-commit', 'local-cancel',
                         'local-success', 'initial-cancel', 'initial-success',
                         'initial-invalid', 'initial-epoch-mismatch'):
                subprocess.run([str(executable), mode], check=True, timeout=15)
        print(f'{name}: PASS', flush=True)
    subprocess.run(['python3', str(COPY / 'sst/tests/device_owners.py')], check=True)
    print('device_owners: PASS', flush=True)
    subprocess.run(['python3', str(COPY / 'sst/tests/test_measurements.py')], check=True)
    print('measurement artifact validation: PASS', flush=True)
    for script in ('test-build-selection.py', 'test-pin-baseline.py',
                   'test-comparison.py', 'test-hardware-runner.py'):
        subprocess.run(['python3', '-B', str(TOOLS / 'tests' / script)], check=True)
    print(f'Executables: {output}')


if __name__ == '__main__':
    main()
