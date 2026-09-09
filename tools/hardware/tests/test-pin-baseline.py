#!/usr/bin/env python3
"""Baseline selection and output ownership, without building hardware."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build import ROOT, digest

spec = importlib.util.spec_from_file_location('pin_baseline', ROOT / 'tools/hardware/pin-baseline.py')
pin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pin)


class BaselineSelectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='golem-pin-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build'
        self.hardware = self.root / 'hardware'
        self.build.mkdir()
        artifacts = {}
        for relative in ('qemu/bin/qemu-system-riscv64',
                         'sst-elements/lib/sst-elements-library/libmittens.so'):
            binary = self.hardware / relative
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(relative.encode())
            artifacts[str(binary)] = digest(binary)
        (self.build / 'build-fixture.json').write_text(json.dumps({
            'completed_unix': 1, 'artifacts': artifacts, 'source_sha256': {}}))
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, {'GOLEM_BUILD_ROOT': str(self.build),
                                'GOLEM_INSTALL_ROOT': str(self.hardware)}, clear=True).start()
        patch.object(pin, 'SOURCE', self.root / 'fixture-source').start()
        patch.object(pin, 'link_dependency').start()

    def capture(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return pin.main(['fixture'])

    def test_override_selects_matching_binaries_and_manifests(self):
        install = self.capture()
        self.assertTrue(install.is_relative_to(self.build / 'baselines'))
        manifest = json.loads((install.parent / 'manifest.json').read_text())
        self.assertEqual(manifest['selected_install_root'], str(self.hardware))
        self.assertEqual(manifest['selected_build_root'], str(self.build))
        for relative, expected in manifest['binary_sha256'].items():
            self.assertEqual(digest(install / relative), expected)
        self.assertEqual(set(manifest['build_provenance'].values()),
                         {str(self.build / 'build-fixture.json')})

    def test_missing_override_provenance_never_falls_back(self):
        with patch.dict(os.environ, {'GOLEM_BUILD_ROOT': str(self.root / 'missing')}):
            with self.assertRaisesRegex(RuntimeError, 'No build provenance'):
                self.capture()
        self.assertFalse((self.build / 'baselines').exists())

    def test_baseline_symlink_cannot_escape_build_root(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (self.build / 'baselines').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'escapes its owner'):
            self.capture()
        self.assertEqual(list(outside.iterdir()), [])

    def test_unsafe_label_rejected_before_output(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            pin.main(['../outside'])
        self.assertFalse((self.build / 'baselines').exists())


if __name__ == '__main__':
    unittest.main()
