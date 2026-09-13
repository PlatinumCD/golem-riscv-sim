#!/usr/bin/env python3
"""Mock orchestration; never acquire or build real dependencies."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BootstrapTests(unittest.TestCase):
    def test_dma_deadline_and_accounting_need_no_runtime_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            llvm = root / 'llvm/bin'
            llvm.mkdir(parents=True)
            for name in ('clang', 'clang++'):
                tool = llvm / name
                tool.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >> "$MOCK_LOG"\n')
                tool.chmod(0o755)
            env = {key: value for key, value in os.environ.items() if not key.startswith('GOLEM_')}
            env.update(GOLEM_LLVM_DIR=str(root / 'llvm'), GOLEM_BUILD_ROOT=str(root / 'build'),
                       GOLEM_INSTALL_ROOT=str(root / 'install'), MOCK_LOG=str(root / 'commands.log'))
            for name in ('cpu_memory_deadline', 'global_dma_clock'):
                result = subprocess.run(['bash', str(ROOT / 'src/sst/tests' / (name + '_build.sh')),
                                         str(root / name)], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('libgolem-runtime', (root / 'commands.log').read_text())
            for name in ('register-only-accounting', 'rvv-only-accounting'):
                result = subprocess.run(['make', '-n', '-C', str(ROOT / 'tests/platform' / name)],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn('libgolem-runtime', result.stdout)

    def test_hardware_platform_targets_do_not_build_or_link_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / 'build-scripts'
            scripts.mkdir()
            shutil.copy2(ROOT / 'build-scripts/build-platform.sh', scripts)
            llvm = root / 'llvm/bin'
            llvm.mkdir(parents=True)
            for name in ('clang', 'clang++', 'llvm-readelf'):
                tool = llvm / name
                tool.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >> "$MOCK_LOG"\n'
                                'if [[ "$1" == --file-header ]]; then echo "Entry point address: ${MOCK_ENTRY:-0x80000000}"; fi\n')
                tool.chmod(0o755)
            (scripts / 'common.sh').write_text(f'''
PROJECT_ROOT={ROOT}
INSTALL_ROOT={root}/install
GOLEM_LLVM_DIR={root}/llvm
TEST_RESULTS_ROOT={root}/results
PLATFORM_ROOT={ROOT}/src/platform/devices
PLATFORM_STARTUP_ROOT={ROOT}/src/platform/startup
GOLEM_TARGET=riscv64-unknown-elf
GOLEM_CPU=golem-analog
GOLEM_ABI=lp64d
require_executable() {{ test -x "$1"; }}
require_sculptor_source() {{ echo unexpected-runtime >&2; exit 71; }}
''')
            log = root / 'commands.log'
            for target in ('hello', 'riscv-vector', 'cpu-timing-validation', 'qemu-ready-set',
                           'scratchpad-dma', 'global-ram-exact', 'global-dma-contention',
                           'epoch-barrier', 'analog-instructions', 'analog-ops', 'analog-timing-validation',
                           'analog-mesh-2x2', 'analog-mesh-2x2-dual-array', 'analog-route-2x2',
                           'mesh-pair', 'mesh-3x3', 'mesh-pipeline'):
                result = subprocess.run(['bash', str(scripts / 'build-platform.sh'), target],
                                        env=dict(os.environ, MOCK_LOG=str(log),
                                                 MOCK_ENTRY='0x90010000'),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, (target, result.stderr))
            self.assertNotIn('/install/runtime/', log.read_text())
            self.assertNotIn('libgolem-runtime', log.read_text())
            for target in ('runtime-library', 'distributed-matvec'):
                result = subprocess.run(['bash', str(scripts / 'build-platform.sh'), target],
                                        env=dict(os.environ, MOCK_LOG=str(log)), capture_output=True, text=True)
                self.assertEqual(result.returncode, 71, result.stderr)

    def test_fresh_hardware_dependency_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / 'build-scripts'
            scripts.mkdir()
            shutil.copy2(ROOT / 'bootstrap.sh', root / 'bootstrap.sh')
            (scripts / 'common.sh').write_text('BUILD_JOBS=2\n')
            for name in ('check-dependencies', 'build-riscv-gnu-toolchain', 'build-llvm',
                         'build-sst-core', 'build-cross-sim', 'build-sst-elements'):
                file = scripts / (name + '.sh')
                file.write_text('#!/bin/bash\nprintf "%s:%s\\n" "${GOLEM_BUILD_SCOPE:-hardware}" "' + name + '"\n')
                file.chmod(0o755)
            bin_dir = root / 'bin'
            bin_dir.mkdir()
            git = bin_dir / 'git'
            git.write_text('#!/bin/bash\nprintf "git %s\\n" "$*"\n')
            git.chmod(0o755)
            result = subprocess.run(['bash', str(root / 'bootstrap.sh'), 'dependencies'],
                                    env=dict(os.environ, PATH=str(bin_dir) + ':' + os.environ['PATH']),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('sculptor-mlir', result.stdout)
            self.assertNotIn('torch-mlir', result.stdout)
            builds = [line for line in result.stdout.splitlines() if line.startswith('shared:')]
            self.assertEqual(builds, ['shared:build-' + name for name in
                                     ('riscv-gnu-toolchain', 'llvm', 'sst-core', 'cross-sim', 'sst-elements')])

    def test_optional_compiler_fails_before_initialization(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith('GOLEM_')}
        result = subprocess.run(['bash', str(ROOT / 'bootstrap.sh'), 'compiler'],
                                env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('GOLEM_SCULPTOR_SOURCE', result.stderr)

    def test_fresh_gnu_acquires_pinned_source_before_configuring(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / 'build-scripts'
            scripts.mkdir()
            shutil.copy2(ROOT / 'build-scripts/build-riscv-gnu-toolchain.sh', scripts)
            (scripts / 'common.sh').write_text('''PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="$PROJECT_ROOT/build"
INSTALL_ROOT="$PROJECT_ROOT/install"
BUILD_JOBS=2
require_owned_comparison_output() { :; }
require_command() { :; }
git() { printf 'git %s\\n' "$*"; }
require_git_commit() { printf 'pin %s\\n' "$2"; }
require_file() { echo 'stop before configure'; exit 73; }
''')
            result = subprocess.run(['bash', str(scripts / 'build-riscv-gnu-toolchain.sh')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 73, result.stderr)
            self.assertIn('clone --no-checkout https://github.com/riscv-collab/riscv-gnu-toolchain.git', result.stdout)
            self.assertIn('checkout --detach aa35d455489235c4984fd2c8c0efbff5948dde5a', result.stdout)


if __name__ == '__main__':
    unittest.main()
