#!/usr/bin/env python3
"""Migration contracts: source ownership, test selection, build integrity and isolation."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
from build import load_module
from hardware_paths import resolve_paths, validate_output_roots
from hardware_suite import selected_cases
COMPONENT = load_module('migration_test_components', ROOT/'src/build.py')
QEMU = load_module('migration_test_qemu', ROOT/'src/components/riscv-qemu/build_qemu.py')
PROFILING = load_module('profiling_controls', ROOT/'src/profiling.py')


class CurrentModelTests(unittest.TestCase):
    def test_canonical_source_and_alias(self):
        self.assertEqual((ROOT/'source_new').resolve(), ROOT/'src')
        self.assertTrue((ROOT/'src/components/mordred/spmEndpoint.cc').is_file())
        self.assertFalse((ROOT/'src/sst').exists())

    def test_qemu_overlay_is_self_contained(self):
        copies=QEMU.copies()
        self.assertTrue(copies)
        for source, _ in copies:
            self.assertTrue(source.is_file(), source)
            self.assertTrue(source.is_relative_to(ROOT/'src/components/riscv-qemu'), source)
        self.assertFalse(any('mittens_analog.c' in target for _, target in copies))

    def test_current_suite_and_retired_names(self):
        cases=selected_cases()
        self.assertEqual(len(cases), len({c.name for c in cases}))
        for case in cases:
            self.assertTrue(case.script.is_file(), case.script)
            self.assertNotIn('src/sst', str(case.script))
        for name in ('network/mesh-3x3', 'network/mordred-spm', 'network/local-spm', 'network/posted-transfers'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                selected_cases(names=[name])
        self.assertEqual([c.name for c in selected_cases('compiler')], ['compiler/sculptor'])

    def test_output_guard_resolves_compatibility_alias(self):
        good=(ROOT/'build/src',ROOT/'install/src',ROOT/'build/src/sources')
        validate_output_roots(*good)
        for path in (ROOT/'src',ROOT/'source_new',ROOT/'source_new/components',ROOT/'third_party',ROOT/'build'):
            with self.assertRaises(ValueError): validate_output_roots(path,good[1],good[2])

    def test_stale_input_and_missing_fixture_rejected(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); source=root/'driver.cc'; source.write_text('source')
            (root/'libtilecomponents.so').write_bytes(b'library')
            manifest=root/'build.json'
            info=dict(plugin=str(root),command=[str(source)],source_sha256={str(source):hashlib.sha256(source.read_bytes()).hexdigest()})
            manifest.write_text(json.dumps(info))
            COMPONENT.load_build_info(manifest, extra_sources=[source])
            with self.assertRaises(RuntimeError): COMPONENT.load_build_info(manifest, with_tests=True)
            source.write_text('changed')
            with self.assertRaises(RuntimeError): COMPONENT.load_build_info(manifest)

    def test_library_identity_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); lib=root/'libtilecomponents.so'; lib.write_bytes(b'changed')
            manifest=root/'build.json'; manifest.write_text(json.dumps(dict(plugin=str(root),command=[],source_sha256={},artifact_sha256={str(lib):'incorrect'})))
            with self.assertRaises(RuntimeError): COMPONENT.load_build_info(manifest)

    def test_cli_lists_current_cases_without_simulator(self):
        result=subprocess.run(['bash',str(ROOT/'tests/run-all.sh'),'--list'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('network/guest-instructions',result.stdout)
        self.assertNotIn('network/posted-transfers',result.stdout)
        self.assertIn('platform/llvm-rvv',result.stdout)
        self.assertNotIn('global-ram',result.stdout)

    def test_paths_and_shared_dependencies(self):
        self.assertEqual(resolve_paths({})['GOLEM_BUILD_ROOT'],str(ROOT/'build/src'))
        self.assertEqual(resolve_paths({'GOLEM_BUILD_SCOPE':'shared'})['GOLEM_INSTALL_ROOT'],str(ROOT/'install'))
        with self.assertRaises(ValueError): resolve_paths({'GOLEM_HARDWARE_TREE':'old_src'})

    def test_cpu_wait_trace_is_distinct_from_queue_and_issue_waits(self):
        analysis = load_module('migration_transfer_analysis', ROOT/'src/tests/llvm-rvv/analysis.py')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('riscv-asq-waits.csv', 'riscv-slq-waits.csv', 'riscv-issue-waits.csv'):
                (root/name).touch()
                with self.assertRaises(AssertionError): analysis.one(root, '-waits.csv')
            cpu = root/'riscv-waits.csv'; cpu.touch()
            self.assertEqual(analysis.one(root, '-waits.csv'), cpu)

    def test_profiling_is_explicitly_enabled(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            output = Path(directory) / 'profiles'
            os.environ['TILE_CYCLE_PROFILE_DIRECTORY'] = str(output)
            self.assertFalse(PROFILING.enabled())
            self.assertIsNone(PROFILING.configure(enabled=False, output_directory=output))
            self.assertFalse(output.exists())
            self.assertNotIn('TILE_CYCLE_PROFILE_DIRECTORY', os.environ)
            self.assertEqual(PROFILING.configure(enabled=True, output_directory=output), output)
            self.assertTrue(PROFILING.enabled())
            self.assertTrue(output.is_dir())
            PROFILING.configure(enabled=False)
            self.assertFalse(PROFILING.enabled())

    def test_profiling_validates_controls_and_test_output(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(ValueError): PROFILING.configure(enabled=True)
            with self.assertRaises(ValueError): PROFILING.configure(enabled='false')
            with self.assertRaises(ValueError): PROFILING.configure(enabled=True, output_directory='')
            for value in ('', 'true', 'false', '2'):
                os.environ['TILE_CYCLE_PROFILE'] = value
                with self.assertRaises(ValueError): PROFILING.enabled()
            os.environ['TILE_COMPONENT_OUTPUT'] = directory
            self.assertEqual(PROFILING.configure(enabled=True), Path(directory) / 'profiles')

    def test_profiling_cli_overrides_inherited_controls(self):
        script = 'import json, os; print(json.dumps({k:v for k,v in os.environ.items() if k.startswith("TILE_CYCLE_PROFILE")}))'
        base = os.environ | dict(TILE_CYCLE_PROFILE='1', TILE_CYCLE_PROFILE_DIRECTORY='/tmp/inherited-profile')
        for options, expected in ((['--no-profile'], {'TILE_CYCLE_PROFILE': '0'}),
                (['--profile', '/tmp/explicit-profile'], {'TILE_CYCLE_PROFILE': '1',
                                                       'TILE_CYCLE_PROFILE_DIRECTORY': '/tmp/explicit-profile'})):
            result = subprocess.run(['bash', str(ROOT/'tools/hardware/env.sh'), *options,
                                     sys.executable, '-c', script], env=base, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), expected)


if __name__=='__main__': unittest.main()
