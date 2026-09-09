#!/usr/bin/env python3
"""Prove that an integrated boot loads src Mittens, QEMU, and guest artifacts."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from hardware_paths import resolve_paths

ROOT = Path(__file__).resolve().parents[2]


def main():
    paths = resolve_paths()
    build, install = Path(paths['GOLEM_BUILD_ROOT']), Path(paths['GOLEM_INSTALL_ROOT'])
    output = build / 'checks' / str(time.time_ns())
    output.mkdir(parents=True)
    elf = build / 'tests/hello/hello.elf'
    qemu = install / 'qemu/bin/qemu-system-riscv64'
    library = install / 'sst-elements/lib/sst-elements-library/libmittens.so'
    env = dict(os.environ, GOLEM_TEST_RESULTS_ROOT=str(build / 'tests'),
               MITTENS_TEST_ELF=str(elf),
               MITTENS_PROVENANCE_PROFILE=str(output / 'profile'),
               GOLEM_RESOLVED_CONFIG_DIR=str(output / 'resolved'))
    with (output / 'guest-build.log').open('w') as log:
        subprocess.run(['bash', str(ROOT / 'tools/hardware/env.sh'), 'bash',
                        str(ROOT / 'build-scripts/build-platform.sh'), 'hello'],
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    command = ['bash', str(ROOT / 'tools/hardware/env.sh'), 'strace', '-f', '-s', '4096',
               '-e', 'trace=execve,openat', '-o', str(output / 'process.trace'),
               str(install / 'sst-core/bin/sst'), str(ROOT / 'src/sst/tests/qemu_boot.py')]
    with (output / 'simulation.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=120)
    trace = (output / 'process.trace').read_text()
    assert f'execve("{qemu}"' in trace, 'SST did not launch isolated QEMU'
    assert str(library) in trace, 'SST did not load isolated Mittens'
    assert str(elf) in trace, 'QEMU did not receive isolated guest image'
    reference = ROOT / 'install/sst-elements/lib/sst-elements-library/libmittens.so'
    assert str(reference) not in trace, 'Reference Mittens was also loaded'
    assert 'single tile booted' in (output / 'simulation.log').read_text(), 'Guest did not print greeting'
    dwarf = subprocess.check_output([str(ROOT / 'install/llvm/bin/llvm-dwarfdump'),
                                     '--debug-info', str(elf)], text=True)
    assert 'src/platform' in dwarf, 'Guest platform provenance is missing'
    result = {'status': 'PASS', 'command': command,
              'artifacts': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in (library, qemu, elf)},
              'shared_sst_core': str((install / 'sst-core/bin/sst').resolve()),
              'trace': str(output / 'process.trace')}
    summary = json.loads((output / 'profile/tile-0-summary.json').read_text())
    result['implementation_id'] = summary['metadata']['implementation_id']['value']
    result['source_tree'] = summary['metadata']['source_tree']['value']
    result['measurement_artifact'] = str(output / 'profile/tile-0-summary.json')
    (output / 'provenance.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Integrated src provenance: PASS ({output})')


if __name__ == '__main__':
    main()
