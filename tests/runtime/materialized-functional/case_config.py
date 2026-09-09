#!/usr/bin/env python3
"""Strict loader for the focused materialized-functional case matrix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


EXPECTED_TOP_LEVEL = {"schema", "version", "cases"}
EXPECTED_CASE_FIELDS = {
    "name",
    "id",
    "mesh",
    "digital_workers",
    "minimum_work_items_per_unit",
    "minimum_epoch_count",
    "minimum_executable_operation_count",
    "minimum_materialized_boundary_count",
    "minimum_full_transfer_count",
    "minimum_tail_transfer_count",
    "required_semantic_layers",
}
OPTIONAL_CASE_FIELDS = {"minimum_explicit_layout_conversion_count"}


def _positive_integer(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def load_cases(path: str | Path) -> dict[str, dict[str, object]]:
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or set(payload) != EXPECTED_TOP_LEVEL:
        raise ValueError("case matrix must contain exactly schema, version, cases")
    if payload["schema"] != "mittens.materialized-functional-cases":
        raise ValueError("unsupported case matrix schema")
    if isinstance(payload["version"], bool) or payload["version"] != 1:
        raise ValueError("unsupported case matrix version")
    records = payload["cases"]
    if not isinstance(records, list) or not records:
        raise ValueError("case matrix must contain a nonempty case list")

    result: dict[str, dict[str, object]] = {}
    ids: set[int] = set()
    for record in records:
        if (
            not isinstance(record, dict)
            or not EXPECTED_CASE_FIELDS.issubset(record)
            or set(record) - EXPECTED_CASE_FIELDS - OPTIONAL_CASE_FIELDS
        ):
            raise ValueError("case record has missing or unsupported fields")
        name = record["name"]
        if not isinstance(name, str) or not name or name in result:
            raise ValueError("case names must be nonempty and unique")
        case_id = _positive_integer(record["id"], f"{name}.id")
        if case_id in ids:
            raise ValueError("case IDs must be unique")
        ids.add(case_id)
        mesh = record["mesh"]
        if not isinstance(mesh, list) or len(mesh) != 2:
            raise ValueError(f"{name}.mesh must contain width and height")
        width = _positive_integer(mesh[0], f"{name}.mesh.width")
        height = _positive_integer(mesh[1], f"{name}.mesh.height")
        workers = _positive_integer(
            record["digital_workers"], f"{name}.digital_workers"
        )
        minimum = _positive_integer(
            record["minimum_work_items_per_unit"],
            f"{name}.minimum_work_items_per_unit",
        )
        if workers > width * height:
            raise ValueError(f"{name}.digital_workers exceeds its mesh")
        minimum_epoch_count = _positive_integer(
            record["minimum_epoch_count"], f"{name}.minimum_epoch_count"
        )
        minimum_executable_operation_count = _positive_integer(
            record["minimum_executable_operation_count"],
            f"{name}.minimum_executable_operation_count",
        )
        minimum_materialized_boundary_count = _positive_integer(
            record["minimum_materialized_boundary_count"],
            f"{name}.minimum_materialized_boundary_count",
        )
        minimum_full_transfer_count = _positive_integer(
            record["minimum_full_transfer_count"],
            f"{name}.minimum_full_transfer_count",
        )
        minimum_tail_transfer_count = _positive_integer(
            record["minimum_tail_transfer_count"],
            f"{name}.minimum_tail_transfer_count",
        )
        required_semantic_layers = record["required_semantic_layers"]
        if (
            not isinstance(required_semantic_layers, list)
            or not required_semantic_layers
            or any(
                not isinstance(layer, str) or not layer
                for layer in required_semantic_layers
            )
            or len(set(required_semantic_layers)) != len(required_semantic_layers)
        ):
            raise ValueError(
                f"{name}.required_semantic_layers must be nonempty and unique"
            )
        minimum_explicit_layout_conversion_count = record.get(
            "minimum_explicit_layout_conversion_count", 0
        )
        if (
            isinstance(minimum_explicit_layout_conversion_count, bool)
            or not isinstance(minimum_explicit_layout_conversion_count, int)
            or minimum_explicit_layout_conversion_count < 0
        ):
            raise ValueError(
                f"{name}.minimum_explicit_layout_conversion_count must be "
                "a nonnegative integer"
            )
        result[name] = {
            "name": name,
            "id": case_id,
            "width": width,
            "height": height,
            "digital_workers": workers,
            "minimum_work_items_per_unit": minimum,
            "minimum_epoch_count": minimum_epoch_count,
            "minimum_executable_operation_count": (
                minimum_executable_operation_count
            ),
            "minimum_materialized_boundary_count": (
                minimum_materialized_boundary_count
            ),
            "minimum_full_transfer_count": minimum_full_transfer_count,
            "minimum_tail_transfer_count": minimum_tail_transfer_count,
            "minimum_explicit_layout_conversion_count": (
                minimum_explicit_layout_conversion_count
            ),
            "required_semantic_layers": tuple(required_semantic_layers),
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("matrix")
    parser.add_argument("case")
    args = parser.parse_args()
    cases = load_cases(args.matrix)
    if args.case not in cases:
        raise SystemExit(f"unknown materialized-functional case: {args.case}")
    case = cases[args.case]
    print(
        "\t".join(
            str(case[field])
            for field in (
                "id",
                "width",
                "height",
                "digital_workers",
                "minimum_work_items_per_unit",
            )
        )
    )


if __name__ == "__main__":
    main()
