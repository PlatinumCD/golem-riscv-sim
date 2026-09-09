#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parents[2]
VALIDATOR = PROJECT_ROOT / "tools" / "compiler" / "validate-sculptor-scalar-region-suite-preflight.py"
FAKE_TOOL = TEST_DIR / "fake-split-tool.py"
MODELS = (
    "resnet32",
    "wide-resnet-16-8",
    "resnet50",
    "yolov9-c",
    "retinanet",
    "rnnt",
    "mobilebert",
    "bert-large",
)


class Phase5SuitePreflightTest(unittest.TestCase):
    def make_campaign(self, root: Path, modes: dict[str, str] | None = None) -> Path:
        modes = modes or {}
        records = []
        for model in MODELS:
            run = root / model
            artifact = run / "deployment" / "08-placed.mlir"
            artifact.parent.mkdir(parents=True)
            artifact.write_text(modes.get(model, "pass") + "\n", encoding="utf-8")
            records.append({"model": model, "run_directory": str(run)})
        campaign = root / "compiler-model-summary.json"
        campaign.write_text(json.dumps({"models": records}), encoding="utf-8")
        return campaign

    def invoke(self, root: Path, campaign: Path) -> subprocess.CompletedProcess[str]:
        report = root / "preflight.json"
        environment = os.environ.copy()
        environment["PHASE5_PREFLIGHT_FAKE_LOG"] = str(root / "fake.log")
        return subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                "--campaign-summary",
                str(campaign),
                "--split-tool",
                str(FAKE_TOOL),
                "--output-json",
                str(report),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )

    def test_all_eight_models_partition_selected_regions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = self.invoke(root, self.make_campaign(root))
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads((root / "preflight.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(
                report["totals"],
                {
                    "eligible_region_count": 16,
                    "fallback_region_count": 8,
                    "malformed_artifact_count": 0,
                    "selected_region_count": 24,
                },
            )
            self.assertEqual([row["model"] for row in report["results"]], list(MODELS))
            invocations = [
                json.loads(line)
                for line in (root / "fake.log").read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(len(invocations), 8)
            for arguments in invocations:
                self.assertIn("--outline-contract-scalar-execution-regions", arguments)
                self.assertIn("--outline-retain-proved-local-owners", arguments)
                self.assertIn("--outline-exact-ram-readiness", arguments)
                self.assertIn("--outline-scalar-summary-only", arguments)

    def test_compiler_rejection_is_malformed_not_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            campaign = self.make_campaign(root, {"retinanet": "compiler-error"})
            completed = self.invoke(root, campaign)
            self.assertEqual(completed.returncode, 1)
            report = json.loads((root / "preflight.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["totals"]["malformed_artifact_count"], 1)
            failed = next(row for row in report["results"] if row["model"] == "retinanet")
            self.assertEqual(failed["status"], "MALFORMED")
            self.assertIsNone(failed["eligible_region_count"])
            self.assertIsNone(failed["fallback_region_count"])
            self.assertIn("stale boundary authority", failed["diagnostic_tail"])
            self.assertEqual(
                len((root / "fake.log").read_text(encoding="utf-8").splitlines()), 8
            )

    def test_stale_schema_and_incoherent_total_fail_closed(self) -> None:
        for mode, fragment in (
            ("stale-schema", "schema is stale"),
            ("incoherent-total", "does not partition selected regions"),
        ):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                completed = self.invoke(
                    root, self.make_campaign(root, {"resnet50": mode})
                )
                self.assertEqual(completed.returncode, 1)
                report = json.loads(
                    (root / "preflight.json").read_text(encoding="utf-8")
                )
                failed = next(
                    row for row in report["results"] if row["model"] == "resnet50"
                )
                self.assertEqual(failed["status"], "MALFORMED")
                self.assertIn(fragment, failed["reason"])
                self.assertIsNone(failed["fallback_region_count"])


if __name__ == "__main__":
    unittest.main()
