#!/usr/bin/env python3
"""Pin current src hardware for the next behavior-preserving refactor gate."""
import argparse
import json
from pathlib import Path
import shutil
import time

from build import ROOT, SOURCE, digest, hardware_inputs, input_identity, link_dependency
from hardware_paths import require_owned_output, resolve_paths


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('label', help='Descriptive stage name; only letters/digits/-/_')
    args = parser.parse_args(argv)
    if not args.label or any(not (c.isascii() and (c.isalnum() or c in '-_'))
                             for c in args.label):
        parser.error('unsafe baseline label')
    paths = resolve_paths()
    build = Path(paths['GOLEM_BUILD_ROOT'])
    hardware = Path(paths['GOLEM_INSTALL_ROOT'])
    manifests = [(p, json.loads(p.read_text())) for p in build.glob('build-*.json')]
    provenances = {}
    for relative, directories in (
        ('qemu/bin/qemu-system-riscv64', ('qemu/', 'bridge/')),
        ('sst-elements/lib/sst-elements-library/libmittens.so', ('sst/', 'bridge/')),
    ):
        binary = hardware / relative
        matching = [(p, m) for p, m in manifests
                    if m.get('artifacts', {}).get(str(binary)) == digest(binary)]
        if not matching:
            raise RuntimeError(f'No build provenance for {binary}')
        provenance, manifest = max(matching, key=lambda item: item[1]['completed_unix'])
        if relative.endswith('libmittens.so') and manifest.get('implementation_id'):
            if input_identity(hardware_inputs()) != manifest['implementation_id']:
                raise RuntimeError('Unbuilt Mittens implementation inputs; rebuild before pinning')
        for name, expected in manifest['source_sha256'].items():
            source = SOURCE / name
            if (name.startswith(directories) and 'tests' not in source.relative_to(SOURCE).parts
                    and source.suffix in ('.h', '.cc', '.cpp', '.c', '.inc', '.S', '.patch', '.am')):
                if not source.is_file() or digest(source) != expected:
                    raise RuntimeError(f'Unbuilt source change: {source}; rebuild before pinning')
        provenances[relative] = str(provenance)
    path = build / 'baselines' / f'{args.label}-{time.time_ns()}'
    require_owned_output(path, build)
    path.mkdir(parents=True, exist_ok=False)
    install = path / 'install'
    artifacts = {}
    for relative in ('qemu/bin/qemu-system-riscv64',
                     'sst-elements/lib/sst-elements-library/libmittens.so'):
        source = hardware / relative
        target = install / relative
        before = digest(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        if digest(target) != before or digest(source) != before:
            raise RuntimeError(f'Hardware changed during baseline capture: {source}')
        artifacts[relative] = before
    for library in ('libmerlin.so', 'libmemHierarchy.so'):
        link_dependency(ROOT / 'install/sst-elements/lib/sst-elements-library' / library,
                        install / 'sst-elements/lib/sst-elements-library' / library)
    manifest = {
        'label': args.label, 'captured_unix': time.time(),
        'source_tree': str(SOURCE), 'hardware_installation': str(install),
        'selected_build_root': str(build), 'selected_install_root': str(hardware),
        'binary_sha256': artifacts,
        'build_provenance': provenances,
        'source_sha256': {str(p.relative_to(SOURCE)): digest(p)
                          for folder in ('sst', 'bridge', 'qemu', 'platform')
                          for p in sorted((SOURCE / folder).rglob('*'))
                          if p.is_file() and '__pycache__' not in p.parts},
        'scope': 'Pinned QEMU/Mittens binaries; shared dependencies and guest/component sources remain selected separately.',
    }
    (path / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(install)
    return install


if __name__ == '__main__':
    main()
