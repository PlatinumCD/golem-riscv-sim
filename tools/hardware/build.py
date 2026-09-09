#!/usr/bin/env python3
"""Build current hardware without changing the shared SST registry.

SST Core, LLVM, Merlin, memHierarchy and CrossSim are shared dependencies.
Mittens and QEMU are built from src; guest artifacts use tools/hardware/env.sh.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
from hardware_paths import resolve_paths, validate_output_roots, require_owned_output

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'src'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hardware_inputs():
    """Implementation inputs only; concurrent documentation/test edits are safe."""
    return {str(p.relative_to(SOURCE)): digest(p)
            for directory in ('sst', 'bridge')
            for p in sorted((SOURCE / directory).rglob('*'))
            if p.is_file() and 'tests' not in p.relative_to(SOURCE).parts
            and p.suffix in ('.h', '.cc', '.cpp', '.c', '.inc', '.am')}


def input_identity(inputs):
    serialized = json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()
    return 'sha256:' + hashlib.sha256(serialized).hexdigest()


def run(command, **kwargs):
    print(shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def link_dependency(source, destination):
    if not source.exists():
        raise RuntimeError(f'Missing shared dependency: {source}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() and destination.resolve() == source.resolve():
        return
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f'Refusing to replace existing dependency: {destination}')
    destination.symlink_to(source, target_is_directory=source.is_dir())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=['sst', 'qemu', 'all'], default='all', nargs='?')
    parser.add_argument('-j', '--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    try:
        paths = resolve_paths()
        build, install, prepared = (Path(paths[key]) for key in
                                   ('GOLEM_BUILD_ROOT', 'GOLEM_INSTALL_ROOT', 'GOLEM_SOURCE_ROOT'))
        validate_output_roots(build, install, prepared)
        require_owned_output(install / 'sst-elements/lib/sst-elements-library/libmittens.so', install)
        require_owned_output(install / 'qemu/bin/qemu-system-riscv64', install)
        require_owned_output(build / 'mittens-objects', build)
    except ValueError as error:
        parser.error(str(error))
    dependencies = [ROOT / 'install' / name for name in (
        'sst-core/bin/sst', 'sst-core/bin/sst-config', 'sst-core/etc/sst/sstsimulator.conf',
        'llvm/bin/clang++', 'cross-sim/python/simulator',
        'sst-elements/lib/sst-elements-library/libmerlin.so',
        'sst-elements/lib/sst-elements-library/libmemHierarchy.so')]
    missing = [str(path) for path in dependencies if not path.exists()]
    if missing:
        parser.error('Missing shared dependencies (no fallback or dependency rebuild): ' + ', '.join(missing))
    build.mkdir(parents=True, exist_ok=True)
    libdir = install / 'sst-elements/lib/sst-elements-library'
    for name in ('sst-core', 'llvm', 'cross-sim'):
        link_dependency(ROOT / 'install' / name, install / name)
    for name in ('libmerlin.so', 'libmemHierarchy.so'):
        link_dependency(ROOT / 'install/sst-elements/lib/sst-elements-library' / name,
                        libdir / name)
    manifest = {'source_tree': str(SOURCE), 'build_root': str(build),
                'install_root': str(install), 'prepared_source_root': str(prepared),
                'started_unix': time.time(), 'commands': [], 'artifacts': {},
                'shared_dependencies': {name: str((install / name).resolve())
                                        for name in ('sst-core', 'llvm', 'cross-sim')}}
    manifest['source_sha256'] = {str(p.relative_to(SOURCE)): digest(p)
                                 for directory in ('sst', 'qemu', 'bridge', 'platform', 'config', 'patches')
                                 for p in sorted((SOURCE / directory).rglob('*'))
                                 if p.is_file() and '__pycache__' not in p.parts}
    manifest['git_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    manifest['git_status'] = subprocess.check_output(['git', 'status', '--porcelain=v1'], cwd=ROOT, text=True)
    manifest['dependency_sha256'] = {str(path.resolve()): digest(path)
                                   for path in dependencies if path.is_file()}
    core = install / 'sst-core'
    registry = core / 'etc/sst/sstsimulator.conf'
    registry_hash = digest(registry)
    if args.component in ('sst', 'all'):
        implementation_inputs = hardware_inputs()
        manifest['implementation_id'] = input_identity(implementation_inputs)
        config = core / 'bin/sst-config'
        def setting(key):
            return shlex.split(subprocess.check_output([str(config), '--' + key], text=True))
        compiler = setting('CXX')
        flags = setting('ELEMENT_CXXFLAGS')
        flags += ['-O3', '-march=native', '-DNDEBUG', '-fPIC',
                  '-DMITTENS_IMPLEMENTATION_ID="' + manifest['implementation_id'] + '"',
                  '-DMITTENS_SOURCE_TREE="src"',
                  '-I' + str(core / 'include/sst/core'),
                  '-I' + str(SOURCE / 'bridge/include')]
        flags += shlex.split(subprocess.check_output(['/usr/bin/python3-config', '--includes'], text=True))
        # Use the production source list rather than accidentally compiling tests.
        makefile = (SOURCE / 'sst/Makefile.am').read_text()
        section = makefile.split('libmittens_la_SOURCES =', 1)[1].split('\n\n', 1)[0]
        sources = [SOURCE / 'sst' / word for word in section.replace('\\', '').split()
                   if word.endswith('.cc')]
        def compile_one(source):
            obj = build / 'mittens-objects' / source.relative_to(SOURCE / 'sst').with_suffix('.o')
            obj.parent.mkdir(parents=True, exist_ok=True)
            command = compiler + flags + ['-c', str(source), '-o', str(obj)]
            run(command)
            return obj, command
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            compiled = list(pool.map(compile_one, sources))
        manifest['commands'].extend(command for _, command in compiled)
        output = libdir / 'libmittens.so'
        temporary = build / 'libmittens.so'
        command = compiler + ['-shared', '-o', str(temporary)] + [str(obj) for obj, _ in compiled]
        run(command)
        manifest['commands'].append(command)
        if hardware_inputs() != implementation_inputs:
            raise RuntimeError('Mittens implementation changed during build; installed library preserved')
        # A successful build alone may replace the comparison library.
        import shutil
        shutil.copy2(temporary, output.with_suffix('.so.new'))
        output.with_suffix('.so.new').replace(output)
        manifest['artifacts'][str(output)] = digest(output)
    if args.component in ('qemu', 'all'):
        env = dict(os.environ, GOLEM_HARDWARE_TREE='src', GOLEM_BUILD_ROOT=str(build),
                   GOLEM_INSTALL_ROOT=str(install), GOLEM_SOURCE_ROOT=str(prepared), JOBS=str(args.jobs))
        command = ['bash', str(ROOT / 'build-scripts/build-qemu.sh')]
        run(command, env=env)
        manifest['commands'].append(command)
        binary = install / 'qemu/bin/qemu-system-riscv64'
        manifest['artifacts'][str(binary)] = digest(binary)
    if digest(registry) != registry_hash:
        raise RuntimeError('Reference SST registry changed during isolated build')
    if any(digest(Path(path)) != expected for path, expected in manifest['dependency_sha256'].items()):
        raise RuntimeError('Shared dependencies changed during hardware build')
    manifest['sst_registry_sha256'] = registry_hash
    manifest['completed_unix'] = time.time()
    path = build / f'build-{args.component}-{time.time_ns()}.json'
    path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Build provenance: {path}', flush=True)


if __name__ == '__main__':
    main()
