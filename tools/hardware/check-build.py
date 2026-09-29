#!/usr/bin/env python3
"""Validate current build/install identities and run a real CPU smoke regression."""
import json
from pathlib import Path
import subprocess
import sys
from build import digest, load_module
from hardware_paths import ROOT, resolve_paths


def main():
    paths=resolve_paths(); build=Path(paths['GOLEM_BUILD_ROOT'])
    manifest=json.loads((build/'current-build.json').read_text())
    for name in ('sst','qemu'):
        component=manifest['components'][name]
        for path, expected in component['artifacts'].items():
            if digest(path)!=expected: raise RuntimeError(f'Installed artifact changed: {path}')
        info=json.loads(Path(component['build_info']).read_text())
        for path, expected in info['source_sha256'].items():
            source=Path(path) if name=='sst' else ROOT/path
            if digest(source)!=expected: raise RuntimeError(f'Build input changed: {source}')
    subprocess.run([sys.executable,'-B',str(ROOT/'tools/hardware/hardware_runner.py'),
                    '--case','platform/riscv-qemu'],check=True)
    print('PASS current installation identities and CPU regression')


if __name__=='__main__': main()
