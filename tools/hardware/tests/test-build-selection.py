#!/usr/bin/env python3
"""Non-mutating checks for source selection and comparison-output safeguards."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build import ROOT, hardware_inputs, input_identity, require_owned_output, validate_output_roots
from hardware_paths import resolve_paths


class BuildSelection(unittest.TestCase):
    def test_shell_test_results_default_and_override(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith('GOLEM_')}
        for override in (None, '/tmp/golem-explicit-test-results'):
            settings = dict(env)
            if override:
                settings['GOLEM_TEST_RESULTS_ROOT'] = override
            result = subprocess.run(['bash', '-c', 'source "$1"; printf "%s" "$TEST_RESULTS_ROOT"',
                                     'test-results', str(ROOT / 'build-scripts/common.sh')],
                                    env=settings, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, override or str(ROOT / 'tests/results'))

    def test_shared_scope_has_explicit_dependency_roots(self):
        paths = resolve_paths({'GOLEM_BUILD_SCOPE': 'shared'})
        self.assertEqual(paths['GOLEM_BUILD_ROOT'], str(ROOT / 'build'))
        self.assertEqual(paths['GOLEM_INSTALL_ROOT'], str(ROOT / 'install'))
        self.assertEqual(paths['GOLEM_SOURCE_ROOT'], str(ROOT / 'build/sources'))

    def shell(self, **settings):
        env = {k: v for k, v in os.environ.items() if not k.startswith('GOLEM_')}
        env.update(settings)
        return subprocess.run(['bash', '-c',
            'source "$1" || exit $?; printf "%s\\n" "$HARDWARE_ROOT" "$BUILD_ROOT" "$INSTALL_ROOT" "$PLATFORM_ROOT"',
            'selection-test', str(ROOT / 'build-scripts/common.sh')],
            env=env, capture_output=True, text=True)

    def test_default_selects_current_hardware(self):
        result = self.shell()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines()[:3],
                         [str(ROOT / 'src'), str(ROOT / 'build/src'), str(ROOT / 'install/src')])

    def test_removed_source_selectors_rejected(self):
        for tree in ('src2', 'old_src'):
            self.assertNotEqual(self.shell(GOLEM_HARDWARE_TREE=tree).returncode, 0)

    def test_explicit_roots_propagate(self):
        with tempfile.TemporaryDirectory(prefix='golem-hardware-roots-') as directory:
            result = self.shell(GOLEM_BUILD_ROOT=directory + '/build', GOLEM_INSTALL_ROOT=directory + '/install')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines()[1:3], [directory + '/build', directory + '/install'])

    def test_comparison_is_isolated(self):
        result = self.shell(GOLEM_HARDWARE_TREE='src')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [str(ROOT / 'src'),
                         str(ROOT / 'build/src'), str(ROOT / 'install/src'),
                         str(ROOT / 'src/platform/devices')])

    def test_reference_roots_rejected(self):
        for key in ('GOLEM_BUILD_ROOT', 'GOLEM_INSTALL_ROOT', 'GOLEM_SOURCE_ROOT'):
            for path in (ROOT, ROOT / 'build', ROOT / 'install', ROOT / 'src',
                         ROOT / 'src/sst', ROOT / 'third_party/qemu', ROOT / 'build/sources'):
                result = self.shell(GOLEM_HARDWARE_TREE='src', **{key: str(path)})
                self.assertNotEqual(result.returncode, 0, (key, path))

    def test_python_guard_matches(self):
        good = (ROOT / 'build/src', ROOT / 'install/src', ROOT / 'build/src/sources')
        validate_output_roots(*good)
        for i in range(3):
            invalid = list(good)
            invalid[i] = ROOT / 'build/sources'
            with self.assertRaises(ValueError):
                validate_output_roots(*invalid)

    def test_symlinks_cannot_redirect_writes(self):
        with tempfile.TemporaryDirectory(prefix='golem-src-selection-') as temporary:
            owner = Path(temporary)
            (owner / 'qemu').symlink_to(ROOT / 'install/qemu', target_is_directory=True)
            with self.assertRaises(ValueError):
                require_owned_output(owner / 'qemu/bin/qemu-system-riscv64', owner)
            (owner / 'sources').symlink_to(ROOT / 'build/sources', target_is_directory=True)
            result = self.shell(GOLEM_HARDWARE_TREE='src', GOLEM_SOURCE_ROOT=str(owner / 'sources'))
            self.assertNotEqual(result.returncode, 0)

    def test_implementation_identity_is_order_independent(self):
        self.assertEqual(input_identity({'a': '1', 'b': '2'}),
                         input_identity({'b': '2', 'a': '1'}))
        self.assertNotEqual(input_identity({'a': '1'}), input_identity({'a': '2'}))

    def test_dependency_builders_reject_shared_install_symlinks_before_building(self):
        with tempfile.TemporaryDirectory(prefix='golem-dependency-guards-') as temporary:
            root = Path(temporary)
            install = root / 'install'
            install.mkdir()
            for dependency in ('sst-core', 'llvm', 'cross-sim'):
                (install / dependency).symlink_to(ROOT / 'install' / dependency, target_is_directory=True)
                script = ROOT / 'build-scripts' / f'build-{dependency}.sh'
                env = dict(os.environ, GOLEM_HARDWARE_TREE='src', GOLEM_INSTALL_ROOT=str(install),
                           GOLEM_BUILD_ROOT=str(root / 'build'), GOLEM_SOURCE_ROOT=str(root / 'sources'))
                result = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('escapes its owner', result.stderr)

    def test_implementation_inputs_detect_added_and_changed_sources(self):
        with tempfile.TemporaryDirectory(prefix='golem-src-inputs-') as temporary:
            source = Path(temporary)
            (source / 'sst/tests').mkdir(parents=True)
            (source / 'bridge').mkdir()
            (source / 'sst/model.cc').write_text('model-v1')
            with patch('build.SOURCE', source):
                initial = hardware_inputs()
                (source / 'sst/tests/test.cpp').write_text('test')
                (source / 'sst/README.md').write_text('documentation')
                self.assertEqual(hardware_inputs(), initial)
                (source / 'bridge/abi.h').write_text('abi')
                self.assertNotEqual(hardware_inputs(), initial)
                with_header = hardware_inputs()
                (source / 'sst/model.cc').write_text('model-v2')
                self.assertNotEqual(hardware_inputs(), with_header)


if __name__ == '__main__':
    unittest.main()
