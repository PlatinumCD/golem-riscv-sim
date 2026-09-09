#!/usr/bin/env python3
"""Check tool entry points without build tools."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]


class EntrypointTest(unittest.TestCase):
    def test_tool_help(self):
        for directory in (ROOT / "tools/analysis", ROOT / "tools/compiler"):
            entries = sorted(directory.glob("*.py"))
            self.assertTrue(entries, str(directory))
            for entry in entries:
                with self.subTest(tool=entry.name):
                    result = subprocess.run(
                        [sys.executable, str(entry), "--help"],
                        cwd="/tmp", capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("usage:", result.stdout)

    def test_import_preserves_repository_root(self):
        for entry in (
            ROOT / "tools/compiler/validate-sculptor-materialization-audit.py",
        ):
            with self.subTest(entry=str(entry)):
                spec = importlib.util.spec_from_file_location("audit_entrypoint", entry)
                module = importlib.util.module_from_spec(spec)
                sys.modules[spec.name] = module
                try:
                    spec.loader.exec_module(module)
                    self.assertEqual(module.PROJECT_ROOT, ROOT)
                    self.assertTrue(callable(module.load_deployment_manifest))
                finally:
                    sys.modules.pop(spec.name, None)


if __name__ == "__main__":
    unittest.main()
