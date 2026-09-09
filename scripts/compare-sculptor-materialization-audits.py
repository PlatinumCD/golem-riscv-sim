#!/usr/bin/env python3
"""Compare two bounded materialization-audit certificates exactly.

The input certificates are small, persistent JSON evidence emitted by
``validate-sculptor-materialization-audit.py``.  This tool never opens the
corresponding MLIR.  It reconciles exact physical-request histograms against
their counters, applies the GlobalRAMController service equation, and records
whether a directional improvement is only an input/output redistribution.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
from pathlib import Path
from typing import Any


UINT64_MAX = (1 << 64) - 1
DIRECTIONS = ("input", "output")
ACCOUNTING_METRICS = (
    "descriptor_count",
    "physical_request_count",
    "physical_byte_count",
    "controller_service_cycles",
)
EXPECTED_AUDIT_SCHEMA = "sculptor.materialization-audit"
EXPECTED_AUDIT_VERSION = 1
EXPECTED_MAXIMUM_FRAME_BYTES = 4096
DEFAULT_MAXIMUM_AUDIT_BYTES = 16 * 1024 * 1024
ABSOLUTE_MAXIMUM_AUDIT_BYTES = 64 * 1024 * 1024


class ComparisonError(RuntimeError):
    """Malformed, incomplete, or arithmetically inconsistent evidence."""


def require_u64(value: Any, context: str) -> int:
    if type(value) is not int or value < 0 or value > UINT64_MAX:
        raise ComparisonError(f"{context} must be an unsigned 64-bit integer")
    return value


def require_positive_u64(value: Any, context: str) -> int:
    result = require_u64(value, context)
    if result == 0:
        raise ComparisonError(f"{context} must be positive")
    return result


def checked_add_u64(left: int, right: int, context: str) -> int:
    require_u64(left, f"left operand while {context}")
    require_u64(right, f"right operand while {context}")
    if left > UINT64_MAX - right:
        raise ComparisonError(f"unsigned 64-bit addition overflow while {context}")
    return left + right


def checked_multiply_u64(left: int, right: int, context: str) -> int:
    require_u64(left, f"left operand while {context}")
    require_u64(right, f"right operand while {context}")
    if left != 0 and right > UINT64_MAX // left:
        raise ComparisonError(
            f"unsigned 64-bit multiplication overflow while {context}"
        )
    return left * right


def ceil_div_u64(numerator: int, denominator: int, context: str) -> int:
    require_u64(numerator, f"numerator while {context}")
    require_positive_u64(denominator, f"denominator while {context}")
    quotient, remainder = divmod(numerator, denominator)
    return checked_add_u64(quotient, int(remainder != 0), context)


def scaled_ratio_floor(
    numerator: int, denominator: int, scale: int, context: str
) -> int | None:
    require_u64(numerator, f"numerator while {context}")
    require_u64(denominator, f"denominator while {context}")
    require_positive_u64(scale, f"scale while {context}")
    if denominator == 0:
        if numerator != 0:
            raise ComparisonError(f"nonzero numerator with zero denominator while {context}")
        return None
    if numerator > denominator:
        raise ComparisonError(f"ratio exceeds one while {context}")
    # Python integers make this exact; bound the representable result just as
    # strictly as every counter and avoid any floating-point loss.
    result = (numerator * scale) // denominator
    return require_u64(result, f"scaled result while {context}")


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ComparisonError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def reject_nonfinite_json(value: str) -> None:
    raise ComparisonError(f"non-finite JSON number {value!r}")


def read_bounded_json(path: Path, maximum_bytes: int) -> tuple[dict[str, Any], dict[str, Any]]:
    maximum_bytes = require_positive_u64(maximum_bytes, "maximum audit byte count")
    if maximum_bytes > ABSOLUTE_MAXIMUM_AUDIT_BYTES:
        raise ComparisonError(
            f"maximum audit byte count exceeds hard bound "
            f"{ABSOLUTE_MAXIMUM_AUDIT_BYTES}"
        )
    try:
        resolved = path.resolve(strict=True)
        with resolved.open("rb") as source:
            metadata = os.fstat(source.fileno())
            if not stat.S_ISREG(metadata.st_mode):
                raise ComparisonError(f"audit evidence is not a regular file: {resolved}")
            if metadata.st_size > maximum_bytes:
                raise ComparisonError(
                    f"audit evidence exceeds {maximum_bytes} bytes: "
                    f"{resolved} has {metadata.st_size}"
                )
            encoded = source.read(maximum_bytes + 1)
            final_metadata = os.fstat(source.fileno())
    except ComparisonError:
        raise
    except OSError as error:
        raise ComparisonError(f"cannot read audit evidence {path}: {error}") from error
    if len(encoded) > maximum_bytes:
        raise ComparisonError(f"audit evidence grew beyond {maximum_bytes} bytes: {resolved}")
    if len(encoded) != metadata.st_size:
        raise ComparisonError(f"audit evidence changed while being read: {resolved}")
    if (
        final_metadata.st_size != metadata.st_size
        or final_metadata.st_mtime_ns != metadata.st_mtime_ns
        or final_metadata.st_ctime_ns != metadata.st_ctime_ns
    ):
        raise ComparisonError(f"audit evidence changed while being read: {resolved}")
    try:
        payload = json.loads(
            encoded.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_nonfinite_json,
        )
    except ComparisonError:
        raise
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ComparisonError(f"malformed audit JSON {resolved}: {error}") from error
    if not isinstance(payload, dict):
        raise ComparisonError(f"audit JSON root must be an object: {resolved}")
    evidence = {
        "path": str(resolved),
        "sha256": hashlib.sha256(encoded).hexdigest(),
        "byte_size": len(encoded),
    }
    return payload, evidence


def parse_histogram(
    value: Any, direction: str, maximum_frame_bytes: int
) -> dict[int, int]:
    if not isinstance(value, dict):
        raise ComparisonError(f"{direction} physical-request histogram must be an object")
    if len(value) > maximum_frame_bytes:
        raise ComparisonError(
            f"{direction} physical-request histogram has too many bins"
        )
    result: dict[int, int] = {}
    for key, raw_count in value.items():
        if not isinstance(key, str) or not key.isascii() or not key.isdigit():
            raise ComparisonError(
                f"{direction} physical-request histogram key {key!r} is not canonical"
            )
        byte_size = int(key)
        if str(byte_size) != key or not 1 <= byte_size <= maximum_frame_bytes:
            raise ComparisonError(
                f"{direction} physical-request histogram key {key!r} is outside "
                f"[1, {maximum_frame_bytes}] or is noncanonical"
            )
        count = require_positive_u64(
            raw_count, f"{direction} physical-request histogram count for {key} bytes"
        )
        result[byte_size] = count
    return result


def service_cycles_for_request(byte_size: int, controller: dict[str, int]) -> int:
    bursts = ceil_div_u64(
        byte_size,
        controller["burst_bytes"],
        f"counting {byte_size}-byte controller bursts",
    )
    transfer = ceil_div_u64(
        byte_size,
        controller["bytes_per_cycle"],
        f"counting {byte_size}-byte controller transfer cycles",
    )
    fixed = checked_multiply_u64(
        bursts,
        controller["fixed_latency_cycles"],
        f"counting {byte_size}-byte fixed-latency cycles",
    )
    return checked_add_u64(
        checked_add_u64(
            controller["setup_cycles"],
            fixed,
            f"counting {byte_size}-byte controller service cycles",
        ),
        transfer,
        f"counting {byte_size}-byte controller service cycles",
    )


def summarize_direction(
    direction: str,
    descriptor_count: int,
    request_count: int,
    physical_bytes: int,
    histogram: dict[int, int],
    controller: dict[str, int],
) -> dict[str, Any]:
    histogram_requests = 0
    histogram_bytes = 0
    service_cycles = 0
    for byte_size, count in histogram.items():
        histogram_requests = checked_add_u64(
            histogram_requests,
            count,
            f"reconciling {direction} histogram requests",
        )
        histogram_bytes = checked_add_u64(
            histogram_bytes,
            checked_multiply_u64(
                byte_size,
                count,
                f"reconciling {direction} histogram bytes",
            ),
            f"reconciling {direction} histogram bytes",
        )
        service_cycles = checked_add_u64(
            service_cycles,
            checked_multiply_u64(
                service_cycles_for_request(byte_size, controller),
                count,
                f"counting {direction} controller service cycles",
            ),
            f"counting {direction} controller service cycles",
        )
    if histogram_requests != request_count:
        raise ComparisonError(
            f"{direction} histogram counts {histogram_requests} requests, "
            f"but counter declares {request_count}"
        )
    if histogram_bytes != physical_bytes:
        raise ComparisonError(
            f"{direction} histogram accounts for {histogram_bytes} bytes, "
            f"but counter declares {physical_bytes}"
        )
    if (request_count == 0) != (physical_bytes == 0):
        raise ComparisonError(
            f"{direction} request and byte counters disagree about empty traffic"
        )
    if (descriptor_count == 0) != (request_count == 0):
        raise ComparisonError(
            f"{direction} descriptor and request counters disagree about empty traffic"
        )
    four_byte_count = histogram.get(4, 0)
    lower_bound = ceil_div_u64(
        service_cycles,
        controller["channels"],
        f"calculating {direction} controller parallel-service lower bound",
    )
    return {
        "descriptor_count": descriptor_count,
        "physical_request_count": request_count,
        "physical_byte_count": physical_bytes,
        "physical_request_size_histogram": {
            str(byte_size): count for byte_size, count in sorted(histogram.items())
        },
        "average_physical_request_bytes": {
            "numerator": physical_bytes,
            "denominator": request_count,
        },
        "four_byte_request_count": four_byte_count,
        "four_byte_request_share": {
            "numerator": four_byte_count,
            "denominator": request_count,
            "parts_per_million_floor": scaled_ratio_floor(
                four_byte_count,
                request_count,
                1_000_000,
                f"calculating {direction} four-byte request share",
            ),
        },
        "controller_service_cycles": service_cycles,
        "controller_parallel_service_cycle_lower_bound": lower_bound,
    }


def combine_histograms(
    left: dict[int, int], right: dict[int, int], context: str
) -> dict[int, int]:
    result = dict(left)
    for byte_size, count in right.items():
        result[byte_size] = checked_add_u64(
            result.get(byte_size, 0), count, context
        )
    return result


def parse_audit(
    payload: dict[str, Any], evidence: dict[str, Any], controller: dict[str, int]
) -> dict[str, Any]:
    if payload.get("schema") != EXPECTED_AUDIT_SCHEMA:
        raise ComparisonError(
            f"audit {evidence['path']} has schema {payload.get('schema')!r}, "
            f"expected {EXPECTED_AUDIT_SCHEMA!r}"
        )
    version = require_u64(
        payload.get("version"), f"audit {evidence['path']} version"
    )
    if version != EXPECTED_AUDIT_VERSION:
        raise ComparisonError(
            f"audit {evidence['path']} has unsupported version {version}"
        )
    if payload.get("status") != "PASS" or payload.get("errors") != []:
        raise ComparisonError(f"audit {evidence['path']} is not a clean PASS certificate")
    maximum_frame_bytes = require_positive_u64(
        payload.get("maximum_frame_bytes"),
        f"audit {evidence['path']} maximum_frame_bytes",
    )
    if maximum_frame_bytes != EXPECTED_MAXIMUM_FRAME_BYTES:
        raise ComparisonError(
            f"audit {evidence['path']} maximum_frame_bytes={maximum_frame_bytes}; "
            f"always-on accounting requires {EXPECTED_MAXIMUM_FRAME_BYTES}"
        )
    counters = payload.get("counters")
    histograms = payload.get("physical_request_size_histograms")
    if not isinstance(counters, dict):
        raise ComparisonError(f"audit {evidence['path']} counters must be an object")
    if not isinstance(histograms, dict) or set(histograms) != set(DIRECTIONS):
        raise ComparisonError(
            f"audit {evidence['path']} must contain exact input/output "
            "physical_request_size_histograms"
        )

    parsed_histograms: dict[str, dict[int, int]] = {}
    summaries: dict[str, dict[str, Any]] = {}
    for direction in DIRECTIONS:
        descriptor_name = f"materialized_{direction}_dma_descriptor_count"
        request_name = f"remaining_{direction}_physical_request_count"
        byte_name = f"remaining_{direction}_physical_byte_count"
        descriptor_count = require_u64(
            counters.get(descriptor_name),
            f"audit {evidence['path']} counter {descriptor_name}",
        )
        request_count = require_u64(
            counters.get(request_name),
            f"audit {evidence['path']} counter {request_name}",
        )
        physical_bytes = require_u64(
            counters.get(byte_name),
            f"audit {evidence['path']} counter {byte_name}",
        )
        histogram = parse_histogram(
            histograms[direction], direction, maximum_frame_bytes
        )
        parsed_histograms[direction] = histogram
        summaries[direction] = summarize_direction(
            direction,
            descriptor_count,
            request_count,
            physical_bytes,
            histogram,
            controller,
        )

    combined_histogram = combine_histograms(
        parsed_histograms["input"],
        parsed_histograms["output"],
        "combining input/output request-size histograms",
    )
    summaries["combined"] = summarize_direction(
        "combined",
        checked_add_u64(
            summaries["input"]["descriptor_count"],
            summaries["output"]["descriptor_count"],
            "combining input/output descriptor counts",
        ),
        checked_add_u64(
            summaries["input"]["physical_request_count"],
            summaries["output"]["physical_request_count"],
            "combining input/output request counts",
        ),
        checked_add_u64(
            summaries["input"]["physical_byte_count"],
            summaries["output"]["physical_byte_count"],
            "combining input/output physical bytes",
        ),
        combined_histogram,
        controller,
    )
    return {"evidence": evidence, "directions": summaries}


def savings_record(baseline: int, candidate: int) -> dict[str, int | str]:
    if candidate < baseline:
        verdict = "improved"
    elif candidate > baseline:
        verdict = "regressed"
    else:
        verdict = "equal"
    return {
        "baseline": baseline,
        "candidate": candidate,
        "candidate_change": candidate - baseline,
        "candidate_savings": baseline - candidate,
        "verdict": verdict,
    }


def opposite_nonzero_signs(left: int, right: int) -> bool:
    return (left < 0 < right) or (right < 0 < left)


def compare_audits(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, Any]:
    comparisons: dict[str, Any] = {}
    for metric in ACCOUNTING_METRICS:
        direction_records = {
            direction: savings_record(
                baseline["directions"][direction][metric],
                candidate["directions"][direction][metric],
            )
            for direction in (*DIRECTIONS, "combined")
        }
        input_savings = direction_records["input"]["candidate_savings"]
        output_savings = direction_records["output"]["candidate_savings"]
        combined_savings = direction_records["combined"]["candidate_savings"]
        redistributed = opposite_nonzero_signs(input_savings, output_savings)
        comparisons[metric] = {
            "directions": direction_records,
            "directionally_redistributed": redistributed,
            "savings_merely_shifted_between_directions": (
                redistributed and combined_savings == 0
            ),
        }

    combined_savings = [
        comparisons[metric]["directions"]["combined"]["candidate_savings"]
        for metric in ACCOUNTING_METRICS
    ]
    any_redistribution = any(
        comparisons[metric]["directionally_redistributed"]
        for metric in ACCOUNTING_METRICS
    )
    shifted_only = any_redistribution and all(value == 0 for value in combined_savings)
    if shifted_only:
        verdict = "direction_shift_only"
    elif all(value == 0 for value in combined_savings):
        verdict = "no_change"
    elif all(value >= 0 for value in combined_savings):
        verdict = "net_savings"
    elif all(value <= 0 for value in combined_savings):
        verdict = "net_regression"
    else:
        verdict = "mixed"
    return {
        "metrics": comparisons,
        "combined_controller_parallel_service_cycle_lower_bound": savings_record(
            baseline["directions"]["combined"][
                "controller_parallel_service_cycle_lower_bound"
            ],
            candidate["directions"]["combined"][
                "controller_parallel_service_cycle_lower_bound"
            ],
        ),
        "combined_four_byte_request_count": savings_record(
            baseline["directions"]["combined"]["four_byte_request_count"],
            candidate["directions"]["combined"]["four_byte_request_count"],
        ),
        "any_directional_redistribution": any_redistribution,
        "savings_merely_shifted_between_directions": shifted_only,
        "verdict": verdict,
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    controller = {
        "channels": require_positive_u64(args.controller_channels, "controller channels"),
        "setup_cycles": require_u64(args.setup_cycles, "controller setup cycles"),
        "bytes_per_cycle": require_positive_u64(
            args.bytes_per_cycle, "controller bytes per cycle"
        ),
        "burst_bytes": require_positive_u64(args.burst_bytes, "controller burst bytes"),
        "fixed_latency_cycles": require_u64(
            args.fixed_latency_cycles, "controller fixed-latency cycles"
        ),
    }
    maximum_audit_bytes = require_positive_u64(
        args.maximum_audit_bytes, "maximum audit byte count"
    )
    baseline_payload, baseline_evidence = read_bounded_json(
        args.baseline_audit, maximum_audit_bytes
    )
    candidate_payload, candidate_evidence = read_bounded_json(
        args.candidate_audit, maximum_audit_bytes
    )
    baseline = parse_audit(baseline_payload, baseline_evidence, controller)
    candidate = parse_audit(candidate_payload, candidate_evidence, controller)
    output_path = args.output.resolve()
    if output_path in {
        Path(baseline_evidence["path"]),
        Path(candidate_evidence["path"]),
    }:
        raise ComparisonError("comparison output must not overwrite input evidence")
    return {
        "schema": "sculptor.materialization-audit-comparison",
        "version": 1,
        "status": "PASS",
        "controller_model": {
            **controller,
            "equation": (
                "setup_cycles + ceil(bytes / burst_bytes) * "
                "fixed_latency_cycles + ceil(bytes / bytes_per_cycle)"
            ),
            "service_equation_source": (
                "src/components/elements/mittens/globalRAMController.cc:613"
            ),
            "parallel_lower_bound_equation": (
                "ceil(sum(per-request service_cycles) / channels)"
            ),
            "parallel_lower_bound_source": (
                "scripts/analyze-residency-feasibility.py:947"
            ),
        },
        "baseline": baseline,
        "candidate": candidate,
        "comparison": compare_audits(baseline, candidate),
        "errors": [],
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-audit", type=Path, required=True)
    parser.add_argument("--candidate-audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--maximum-audit-bytes", type=int, default=DEFAULT_MAXIMUM_AUDIT_BYTES
    )
    parser.add_argument("--controller-channels", type=int, default=32)
    parser.add_argument("--setup-cycles", type=int, default=8)
    parser.add_argument("--bytes-per-cycle", type=int, default=32)
    parser.add_argument("--burst-bytes", type=int, default=64)
    parser.add_argument("--fixed-latency-cycles", type=int, default=2)
    args = parser.parse_args()

    try:
        report = build_report(args)
    except ComparisonError as error:
        report = {
            "schema": "sculptor.materialization-audit-comparison",
            "version": 1,
            "status": "FAIL",
            "baseline_audit": str(args.baseline_audit.resolve()),
            "candidate_audit": str(args.candidate_audit.resolve()),
            "errors": [str(error)],
        }
        write_report(args.output, report)
        print(f"materialization audit comparison failed: {error}", file=sys.stderr)
        return 1

    write_report(args.output, report)
    combined = report["comparison"]["metrics"]["physical_request_count"][
        "directions"
    ]["combined"]
    print(
        "materialization audit comparison PASS: "
        f"requests={combined['baseline']}->{combined['candidate']} "
        f"verdict={report['comparison']['verdict']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
