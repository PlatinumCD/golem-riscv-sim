#!/usr/bin/env python3
"""Build and install the current component simulator and its matching QEMU."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from hardware_paths import ROOT, resolve_paths, validate_output_roots, require_owned_output

SOURCE = ROOT / 'src'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def install_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.new')
    shutil.copy2(source, temporary)
    temporary.replace(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=['sst', 'qemu', 'all'], nargs='?', default='all')
    parser.add_argument('-j', '--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    paths = resolve_paths()
    build, install, prepared = (Path(paths[key]) for key in
        ('GOLEM_BUILD_ROOT', 'GOLEM_INSTALL_ROOT', 'GOLEM_SOURCE_ROOT'))
    validate_output_roots(build, install, prepared)
    for destination in (build / "components", build / "qemu", install / "lib", install / "qemu", install / "bin", install / "share"):
        require_owned_output(destination, build if destination.is_relative_to(build) else install)
    build.mkdir(parents=True, exist_ok=True)
    manifest_path = build / 'current-build.json'
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    manifest = dict(schema='golem.component-build', source_tree=str(SOURCE), build_root=str(build),
                    install_root=str(install), components=previous.get('components', {}))
    if args.component in ('sst', 'all'):
        component = load_module('current_components_build', SOURCE / 'build.py')
        info = component.build(build / 'components')
        for name in ('libtilecomponents.so', 'libmordred.so'):
            install_file(build / 'components' / name, install / 'lib' / name)
        install_file(build / 'components/build.json', install / 'share/components-build.json')
        manifest['components']['sst'] = dict(build_info=str(build / 'components/build.json'),
            artifacts={str(install / 'lib' / name): digest(install / 'lib' / name)
                       for name in ('libtilecomponents.so', 'libmordred.so')})
        launcher = install / 'bin/sst'
        launcher.parent.mkdir(parents=True, exist_ok=True)
        import shlex
        launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(info['sst']) +
            ' --add-lib-path=' + shlex.quote(str(install / 'lib')) + ' "$@"\n')
        launcher.chmod(0o755)
    if args.component in ('qemu', 'all'):
        qemu = load_module('current_qemu_build', SOURCE / 'components/riscv-qemu/build_qemu.py')
        binary = qemu.build(build / 'qemu', args.jobs)
        destination = install / 'qemu/bin/qemu-system-riscv64'
        install_file(binary, destination)
        install_file(build / 'qemu/build.json', install / 'share/qemu-build.json')
        manifest['components']['qemu'] = dict(build_info=str(build / 'qemu/build.json'),
            artifacts={str(destination): digest(destination)})
    manifest['git_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Current source: {SOURCE}\nBuild manifest: {manifest_path}\nInstallation: {install}', flush=True)


if __name__ == '__main__':
    main()
