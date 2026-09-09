#!/usr/bin/env python3
"""Canonical hardware paths, shared by build, environment and test entry points."""
import argparse
import json
import os
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parents[2]


def validate_output_roots(build, install, prepared):
    for path in (build, install, prepared):
        canonical = Path(path).resolve()
        if canonical in (ROOT, ROOT / 'build', ROOT / 'install', Path('/')) or any(
            canonical.is_relative_to(forbidden) for forbidden in
            (ROOT / 'src', ROOT / 'third_party', ROOT / 'build/sources')):
            raise ValueError(f'unsafe hardware output/prepared-source root: {path}')


def require_owned_output(destination, owner):
    if not Path(destination).resolve().is_relative_to(Path(owner).resolve()):
        raise ValueError(f'hardware output escapes its owner through a symlink: {destination}')


def resolve_paths(environment=None):
    env = os.environ if environment is None else environment
    tree = env.get('GOLEM_HARDWARE_TREE') or 'src'
    if tree != 'src':
        raise ValueError(f'unsupported GOLEM_HARDWARE_TREE: {tree}; active source is src')
    build = Path(env.get('GOLEM_BUILD_ROOT') or ROOT / 'build/src').resolve()
    install = Path(env.get('GOLEM_INSTALL_ROOT') or ROOT / 'install/src').resolve()
    prepared = Path(env.get('GOLEM_SOURCE_ROOT') or build / 'sources').resolve()
    validate_output_roots(build, install, prepared)
    return {'GOLEM_HARDWARE_TREE': 'src', 'GOLEM_BUILD_ROOT': str(build),
            'GOLEM_INSTALL_ROOT': str(install), 'GOLEM_SOURCE_ROOT': str(prepared),
            'GOLEM_LLVM_DIR': str(Path(env.get('GOLEM_LLVM_DIR') or ROOT / 'install/llvm').resolve())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell', action='store_true')
    args = parser.parse_args()
    try:
        paths = resolve_paths()
    except ValueError as error:
        parser.error(str(error))
    if args.shell:
        print('\n'.join(f'export {key}={shlex.quote(value)}' for key, value in paths.items()))
    else:
        print(json.dumps(paths, sort_keys=True))


if __name__ == '__main__':
    main()
