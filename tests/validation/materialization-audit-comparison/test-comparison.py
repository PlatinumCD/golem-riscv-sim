#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[3]
COMPARATOR = PROJECT_ROOT / "tools" / "analysis" / "compare-sculptor-materialization-audits.py"
UINT64_MAX = (1 << 64) - 1


def audit(
    input_histogram: dict[str, int],
    output_histogram: dict[str, int],
    *,
    input_descriptors: int | None = None,
    output_descriptors: int | None = None,
) -> dict[str, Any]:
    def totals(histogram: dict[str, int]) -> tuple[int, int]:
        return sum(histogram.values()), sum(
            int(byte_size) * count for byte_size, count in histogram.items()
        )

    input_requests, input_bytes = totals(input_histogram)
    output_requests, output_bytes = totals(output_histogram)
    return {
        "schema": "sculptor.materialization-audit",
        "version": 1,
        "status": "PASS",
        "maximum_frame_bytes": 4096,
        "counters": {
            "materialized_input_dma_descriptor_count": (
                input_requests if input_descriptors is None else input_descriptors
            ),
            "materialized_output_dma_descriptor_count": (
                output_requests if output_descriptors is None else output_descriptors
            ),
            "remaining_input_physical_request_count": input_requests,
            "remaining_output_physical_request_count": output_requests,
            "remaining_input_physical_byte_count": input_bytes,
            "remaining_output_physical_byte_count": output_bytes,
        },
        "physical_request_size_histograms": {
            "input": input_histogram,
            "output": output_histogram,
        },
        "errors": [],
    }


class MaterializationAuditComparisonTest(unittest.TestCase):
    def run_comparator(
        self,
        root: Path,
        baseline: dict[str, Any] | str,
        candidate: dict[str, Any] | str,
        *extra: str,
    ) -> tuple[subprocess.CompletedProcess[str], Path, Path, Path]:
        baseline_path = root / "baseline-audit.json"
        candidate_path = root / "candidate-audit.json"
        output_path = root / "comparison.json"
        baseline_path.write_text(
            baseline if isinstance(baseline, str) else json.dumps(baseline),
            encoding="utf-8",
        )
        candidate_path.write_text(
            candidate if isinstance(candidate, str) else json.dumps(candidate),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                "python3",
                str(COMPARATOR),
                "--baseline-audit",
                str(baseline_path),
                "--candidate-audit",
                str(candidate_path),
                "--output",
                str(output_path),
                *extra,
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        return result, baseline_path, candidate_path, output_path

    def test_exact_accounting_service_equation_and_immutable_evidence(self) -> None:
        baseline = audit({"4": 10, "64": 2}, {"4096": 1})
        candidate = audit({"4": 2, "64": 2}, {"4096": 1})
        with tempfile.TemporaryDirectory() as temporary:
            result, baseline_path, _, output_path = self.run_comparator(
                Path(temporary), baseline, candidate
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["comparison"]["verdict"], "net_savings")
            combined = report["baseline"]["directions"]["combined"]
            self.assertEqual(combined["descriptor_count"], 13)
            self.assertEqual(combined["physical_request_count"], 13)
            self.assertEqual(combined["physical_byte_count"], 4264)
            self.assertEqual(
                combined["physical_request_size_histogram"],
                {"4": 10, "64": 2, "4096": 1},
            )
            self.assertEqual(combined["four_byte_request_count"], 10)
            self.assertEqual(
                combined["four_byte_request_share"]["parts_per_million_floor"],
                769230,
            )
            # Per request: 4 B -> 8 + 1*2 + 1 = 11 cycles;
            # 64 B -> 12 cycles; 4096 B -> 264 cycles.
            self.assertEqual(combined["controller_service_cycles"], 398)
            self.assertEqual(
                combined["controller_parallel_service_cycle_lower_bound"], 13
            )
            self.assertEqual(
                report["candidate"]["directions"]["combined"][
                    "controller_service_cycles"
                ],
                310,
            )
            self.assertEqual(
                report["comparison"]["metrics"]["physical_request_count"][
                    "directions"
                ]["combined"]["candidate_savings"],
                8,
            )
            self.assertEqual(
                report["comparison"][
                    "combined_controller_parallel_service_cycle_lower_bound"
                ]["candidate_savings"],
                3,
            )
            self.assertEqual(
                report["comparison"]["combined_four_byte_request_count"][
                    "candidate_savings"
                ],
                8,
            )
            encoded = baseline_path.read_bytes()
            self.assertEqual(
                report["baseline"]["evidence"]["path"],
                str(baseline_path.resolve()),
            )
            self.assertEqual(
                report["baseline"]["evidence"]["sha256"],
                hashlib.sha256(encoded).hexdigest(),
            )
            self.assertEqual(
                report["baseline"]["evidence"]["byte_size"], len(encoded)
            )

    def test_equal_combined_work_is_reported_as_direction_shift_only(self) -> None:
        baseline = audit(
            {"4": 10}, {"64": 2}, input_descriptors=10, output_descriptors=2
        )
        candidate = audit(
            {"4": 2},
            {"4": 8, "64": 2},
            input_descriptors=2,
            output_descriptors=10,
        )
        with tempfile.TemporaryDirectory() as temporary:
            result, _, _, output_path = self.run_comparator(
                Path(temporary), baseline, candidate
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            comparison = report["comparison"]
            self.assertEqual(comparison["verdict"], "direction_shift_only")
            self.assertTrue(comparison["any_directional_redistribution"])
            self.assertTrue(
                comparison["savings_merely_shifted_between_directions"]
            )
            for metric in (
                "descriptor_count",
                "physical_request_count",
                "physical_byte_count",
                "controller_service_cycles",
            ):
                self.assertEqual(
                    comparison["metrics"][metric]["directions"]["combined"][
                        "candidate_savings"
                    ],
                    0,
                )
                self.assertTrue(
                    comparison["metrics"][metric][
                        "savings_merely_shifted_between_directions"
                    ]
                )

    def test_malformed_or_missing_evidence_fails_closed(self) -> None:
        valid = audit({"4": 1}, {"4096": 1})
        cases: dict[str, tuple[dict[str, Any] | str, tuple[str, ...]]] = {}

        missing_histogram = copy.deepcopy(valid)
        del missing_histogram["physical_request_size_histograms"]
        cases["missing histogram"] = (missing_histogram, ())

        missing_counter = copy.deepcopy(valid)
        del missing_counter["counters"]["remaining_input_physical_request_count"]
        cases["missing counter"] = (missing_counter, ())

        count_mismatch = copy.deepcopy(valid)
        count_mismatch["counters"]["remaining_input_physical_request_count"] = 2
        cases["histogram count mismatch"] = (count_mismatch, ())

        byte_mismatch = copy.deepcopy(valid)
        byte_mismatch["counters"]["remaining_input_physical_byte_count"] = 5
        cases["histogram byte mismatch"] = (byte_mismatch, ())

        noncanonical_key = copy.deepcopy(valid)
        noncanonical_key["physical_request_size_histograms"]["input"] = {"04": 1}
        cases["noncanonical histogram key"] = (noncanonical_key, ())

        oversized_request = copy.deepcopy(valid)
        oversized_request["physical_request_size_histograms"]["input"] = {"4097": 1}
        oversized_request["counters"]["remaining_input_physical_byte_count"] = 4097
        cases["request exceeds frame"] = (oversized_request, ())

        boolean_count = copy.deepcopy(valid)
        boolean_count["physical_request_size_histograms"]["input"] = {"4": True}
        cases["boolean count"] = (boolean_count, ())

        overflow = copy.deepcopy(valid)
        overflow["physical_request_size_histograms"]["input"] = {
            "4096": UINT64_MAX
        }
        overflow["counters"]["remaining_input_physical_request_count"] = UINT64_MAX
        overflow["counters"]["remaining_input_physical_byte_count"] = UINT64_MAX
        cases["checked arithmetic overflow"] = (overflow, ())

        failed_status = copy.deepcopy(valid)
        failed_status["status"] = "FAIL"
        failed_status["errors"] = ["prior failure"]
        cases["non-PASS input"] = (failed_status, ())

        wrong_frame = copy.deepcopy(valid)
        wrong_frame["maximum_frame_bytes"] = 8192
        cases["not always-on 4 KiB"] = (wrong_frame, ())

        cases["bounded input"] = (valid, ("--maximum-audit-bytes", "32"))
        cases["duplicate JSON key"] = (
            '{"schema":"sculptor.materialization-audit",'
            '"schema":"sculptor.materialization-audit"}',
            (),
        )

        for name, (candidate, extra) in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                result, _, _, output_path = self.run_comparator(
                    Path(temporary), valid, candidate, *extra
                )
                self.assertNotEqual(result.returncode, 0)
                report = json.loads(output_path.read_text(encoding="utf-8"))
                self.assertEqual(report["status"], "FAIL")
                self.assertTrue(report["errors"])


if __name__ == "__main__":
    unittest.main()
