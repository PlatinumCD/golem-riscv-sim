#!/usr/bin/env python3
"""Fail closed on the compiler structure required by one M4 fixture."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from case_config import load_cases


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))
from deployment_manifest import load_deployment_manifest  # noqa: E402


def read_active(path: Path) -> list[int]:
    values = [
        int(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line
    ]
    if not values or values != sorted(set(values)):
        raise ValueError("active-core manifest must be sorted, unique, and nonempty")
    return values


def bracketed(text: str, marker: str, opening: str, closing: str) -> str:
    marker_index = text.find(marker)
    if marker_index < 0:
        raise ValueError(f"missing compiler structure: {marker}")
    start = text.find(opening, marker_index + len(marker))
    if start < 0:
        raise ValueError(f"missing {opening} after compiler structure: {marker}")
    depth = 0
    quoted = False
    escaped = False
    for index in range(start, len(text)):
        character = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
        elif character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return text[start + 1 : index]
    raise ValueError(f"unterminated compiler structure: {marker}")


def integer_field(structure: str, field: str) -> int:
    matches = re.findall(rf"\b{re.escape(field)} = (\d+) : i64\b", structure)
    if len(matches) != 1:
        raise ValueError(f"compiler structure must contain one {field}")
    return int(matches[0])


def validate(
    *,
    case_name: str,
    matrix: Path,
    formed: Path,
    active_manifest: Path,
    deployment_manifest: Path,
    extracted_directory: Path,
) -> dict[str, int]:
    cases = load_cases(matrix)
    if case_name not in cases:
        raise ValueError(f"unknown materialized-functional case: {case_name}")
    case = cases[case_name]
    text = formed.read_text(encoding="utf-8")

    epoch_matches = re.findall(
        r"sculptor\.materialization\.epoch_count = (\d+) : i64\b", text
    )
    if len(epoch_matches) != 1:
        raise ValueError("formed module must contain one materialization epoch count")
    epoch_count = int(epoch_matches[0])
    if epoch_count < case["minimum_epoch_count"]:
        raise ValueError("materialization epoch schedule is shallower than the case")

    operation_epochs = bracketed(
        text, "sculptor.materialization.operation_epochs =", "[", "]"
    )
    executable_operations = len(re.findall(r"\bexecutable = true\b", operation_epochs))
    if executable_operations < case["minimum_executable_operation_count"]:
        raise ValueError("production pipeline erased required executable operations")

    boundaries = bracketed(
        text, "sculptor.materialization.boundaries =", "[", "]"
    )
    boundary_count = len(re.findall(r"\bboundary_id = \d+ : i64\b", boundaries))
    if boundary_count < case["minimum_materialized_boundary_count"]:
        raise ValueError("production pipeline erased required materialized boundaries")

    for semantic_layer in case["required_semantic_layers"]:
        token = f'sculptor.semantic.layer_kind = "{semantic_layer}"'
        if token not in text:
            raise ValueError(f"required semantic layer did not survive: {semantic_layer}")

    audit = bracketed(text, "sculptor.materialization.audit =", "{", "}")
    if integer_field(audit, "cross_epoch_direct_route_count") != 0:
        raise ValueError("materialized fixture retains a cross-epoch direct route")
    full_transfers = integer_field(
        audit, "materialized_main_transfer_count_logical"
    )
    tail_transfers = integer_field(
        audit, "materialized_tail_transfer_count_logical"
    )
    if full_transfers < case["minimum_full_transfer_count"]:
        raise ValueError("compiler audit contains no required exact 4096-byte transfer")
    if tail_transfers < case["minimum_tail_transfer_count"]:
        raise ValueError("compiler audit contains no required tail transfer")

    layout_audit = bracketed(text, "sculptor.sharding.layout_audit =", "{", "}")
    explicit_conversions = integer_field(
        layout_audit, "explicit_layout_conversion_count"
    )
    if explicit_conversions < case["minimum_explicit_layout_conversion_count"]:
        raise ValueError("required explicit layout conversion was not inserted")

    active = read_active(active_manifest)
    deployment = load_deployment_manifest(
        deployment_manifest,
        network_size=case["width"] * case["height"],
        expected_active_tiles=active,
    )
    if deployment["epoch_count"] != epoch_count:
        raise ValueError("deployment epoch count disagrees with the formed module")

    extracted_text = ""
    for tile in active:
        artifact = extracted_directory / f"core-{tile}-extracted.mlir"
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise ValueError(f"active tile {tile} has no extracted module")
        extracted_text += artifact.read_text(encoding="utf-8")
    direct_routes = extracted_text.count("#sculptor.tile_routine_route<")
    if direct_routes != 0:
        raise ValueError("materialized fixture emitted an outlined direct route")

    return {
        "active_tiles": len(active),
        "epoch_count": epoch_count,
        "executable_operations": executable_operations,
        "boundaries": boundary_count,
        "full_transfers": full_transfers,
        "tail_transfers": tail_transfers,
        "explicit_conversions": explicit_conversions,
        "direct_routes": direct_routes,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--formed", type=Path, required=True)
    parser.add_argument("--active-cores", type=Path, required=True)
    parser.add_argument("--deployment-manifest", type=Path, required=True)
    parser.add_argument("--extracted-directory", type=Path, required=True)
    args = parser.parse_args()
    result = validate(
        case_name=args.case,
        matrix=args.matrix,
        formed=args.formed,
        active_manifest=args.active_cores,
        deployment_manifest=args.deployment_manifest,
        extracted_directory=args.extracted_directory,
    )
    fields = " ".join(f"{name}={value}" for name, value in result.items())
    print(f"MATERIALIZED_FUNCTIONAL_STRUCTURE case={args.case} {fields}")


if __name__ == "__main__":
    main()
