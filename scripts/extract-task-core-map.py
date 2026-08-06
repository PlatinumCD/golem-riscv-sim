#!/usr/bin/env python3
"""Extract scheduled Sculptor task placement into a stable CSV artifact."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


STRING_ATTRIBUTES = ("task_name", "task_kind", "source_layer")
INTEGER_ATTRIBUTES = (
    "source_task_ordinal",
    "sculptor.runtime.core_id",
    "sculptor.runtime.local_array_id",
    "sculptor.runtime.physical_array_id",
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def string_attribute(line: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\"([^\"]*)\"", line)
    return match.group(1) if match else ""


def integer_attribute(line: str, name: str) -> str:
    match = re.search(
        rf"\b{re.escape(name)}\s*=\s*(-?\d+)"
        rf"(?:\s*:\s*i64)?(?=\s*[,}}])",
        line,
    )
    return match.group(1) if match else ""


def required_mode(text: str, attribute: str) -> str:
    match = re.search(
        rf'\b{re.escape(attribute)}\s*=\s*"([^"]+)"',
        text,
    )
    if not match:
        raise ValueError(f"scheduled module has no {attribute}")
    return match.group(1)


def main() -> None:
    arguments = parse_arguments()
    text = arguments.input.read_text(encoding="utf-8")
    timing_mode = required_mode(text, "sculptor.timing.mvm_cost_mode")
    placement_match = re.search(
        r'\bsculptor\.schedule\.placement_cost_mode\s*=\s*"([^"]+)"',
        text,
    )
    placement_mode = placement_match.group(1) if placement_match else "n/a"
    rows: list[list[str]] = []

    for line in text.splitlines():
        if "sculptor.task.create" not in line:
            continue
        fields = [string_attribute(line, name) for name in STRING_ATTRIBUTES]
        fields.extend(integer_attribute(line, name) for name in INTEGER_ATTRIBUTES)
        if not fields[3]:
            raise ValueError(f"scheduled task has no source ordinal: {fields[0]!r}")
        if not fields[4]:
            raise ValueError(f"scheduled task has no core assignment: {fields[0]!r}")
        rows.append([*fields, timing_mode, placement_mode])

    if not rows:
        raise ValueError("scheduled module contains no Sculptor tasks")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = arguments.output.with_suffix(arguments.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                *STRING_ATTRIBUTES,
                *INTEGER_ATTRIBUTES,
                "timing_mvm_cost_mode",
                "placement_cost_mode",
            ]
        )
        writer.writerows(rows)
    temporary.replace(arguments.output)
    print(f"wrote {len(rows)} task placements to {arguments.output}")


if __name__ == "__main__":
    main()
