#!/usr/bin/env python3
"""Validate the compiler V1 architecture dictionary and emit its manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


FIELDS = (
    "version",
    "fixed_shard_bytes",
    "scratchpad_bytes",
    "global_ram_bytes",
    "max_in_flight",
    "noc_word_bytes",
)


def parse_dictionary(module: str) -> dict[str, int]:
    matches = re.findall(
        r"sculptor\.arch\.streaming\s*=\s*\{([^{}]*)\}",
        module,
        flags=re.DOTALL,
    )
    if len(matches) != 1:
        raise ValueError(
            "expected exactly one sculptor.arch.streaming dictionary, "
            f"found {len(matches)}"
        )
    values = {
        name: int(value)
        for name, value in re.findall(
            r"([a-z_]+)\s*=\s*(-?[0-9]+)\s*:\s*i64",
            matches[0],
        )
    }
    missing = [field for field in FIELDS if field not in values]
    extra = sorted(set(values).difference(FIELDS))
    if missing or extra:
        raise ValueError(
            f"invalid streaming architecture fields: missing={missing}, extra={extra}"
        )
    return {field: values[field] for field in FIELDS}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mlir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixed-shard-bytes", type=int, required=True)
    parser.add_argument("--scratchpad-bytes", type=int, required=True)
    parser.add_argument("--global-ram-bytes", type=int, required=True)
    parser.add_argument("--max-in-flight", type=int, required=True)
    args = parser.parse_args()

    actual = parse_dictionary(args.mlir.read_text())
    expected = {
        "version": 1,
        "fixed_shard_bytes": args.fixed_shard_bytes,
        "scratchpad_bytes": args.scratchpad_bytes,
        "global_ram_bytes": args.global_ram_bytes,
        "max_in_flight": args.max_in_flight,
        "noc_word_bytes": 4,
    }
    if actual != expected:
        differences = ", ".join(
            f"{field}: compiler={actual[field]} expected={expected[field]}"
            for field in FIELDS
            if actual[field] != expected[field]
        )
        raise SystemExit(f"streaming architecture mismatch: {differences}")

    manifest = {
        "schema": "golem.streaming-architecture",
        "schema_version": 1,
        "source_mlir": str(args.mlir.resolve()),
        "architecture": actual,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
