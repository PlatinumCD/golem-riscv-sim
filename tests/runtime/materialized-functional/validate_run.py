#!/usr/bin/env python3
"""Reconcile guest-derived ABI accounting with physical SST counters."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re


ACCOUNT = re.compile(
    r"MATERIALIZED_FUNCTIONAL_ACCOUNT case=(?P<case>\S+) "
    r"tile=(?P<tile>\d+) iterations=(?P<iterations>\d+) "
    r"full_requests=(?P<full>\d+) tail_requests=(?P<tail>\d+) "
    r"runtime_requests=(?P<runtime_requests>\d+) "
    r"runtime_bytes=(?P<runtime_bytes>\d+) "
    r"validation_requests=(?P<validation_requests>\d+) "
    r"validation_bytes=(?P<validation_bytes>\d+)"
)
SCRATCHPAD = re.compile(
    r"MITTENS_SCRATCHPAD_PROFILE tile=(?P<tile>\d+) .*?"
    r"dma_transfers=(?P<requests>\d+) dma_bytes=(?P<bytes>\d+)"
)
PASS = re.compile(
    r"^MATERIALIZED_FUNCTIONAL_PASS case=(?P<case>\S+) "
    r"tile=(?P<tile>\d+)$",
    re.MULTILINE,
)


def read_active(path: Path) -> list[int]:
    values = [
        int(line.strip())
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if not values or values != sorted(set(values)):
        raise ValueError("active-core manifest must be sorted, unique, and nonempty")
    return values


def one_per_tile(pattern: re.Pattern[str], text: str, active: list[int]):
    result = {}
    for match in pattern.finditer(text):
        tile = int(match.group("tile"))
        if tile in result:
            raise ValueError(f"duplicate {pattern.pattern[:24]} record for tile {tile}")
        result[tile] = match
    if set(result) != set(active):
        raise ValueError(
            f"recorded tile set {sorted(result)} does not match active {active}"
        )
    return result


def statistic_totals(path: Path) -> dict[str, int]:
    totals: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row.get("ComponentName") != "global_ram":
                continue
            name = row.get("StatisticName")
            if name in ("requests", "bytes"):
                if name in totals:
                    raise ValueError(
                        f"duplicate global RAM {name} statistic"
                    )
                totals[name] = int(row["Sum.u64"])
    if set(totals) != {"requests", "bytes"}:
        raise ValueError("missing global RAM request/byte statistics")
    return totals


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--statistics", type=Path, required=True)
    parser.add_argument("--active-cores", type=Path, required=True)
    args = parser.parse_args()

    active = read_active(args.active_cores)
    text = args.log.read_text(encoding="utf-8", errors="replace").replace("\r", "")
    forbidden = (
        "MATERIALIZED_FUNCTIONAL_ERROR",
        "SCULPTOR_RA_SIM_ERROR",
        "MITTENS_FATAL",
        "panic",
        "assertion failed",
    )
    lowered = text.lower()
    if any(token.lower() in lowered for token in forbidden):
        raise ValueError("simulation log contains a tile/runtime error")

    accounts = one_per_tile(ACCOUNT, text, active)
    profiles = one_per_tile(SCRATCHPAD, text, active)
    passes = one_per_tile(PASS, text, active)
    expected_requests = 0
    expected_bytes = 0
    full = 0
    tails = 0
    validation_tiles = []
    for tile in active:
        account = accounts[tile]
        if account.group("case") != args.case:
            raise ValueError(f"tile {tile} reported the wrong case")
        fields = {name: int(account.group(name)) for name in (
            "iterations", "full", "tail", "runtime_requests",
            "runtime_bytes", "validation_requests", "validation_bytes"
        )}
        if fields["iterations"] <= 0:
            raise ValueError(f"tile {tile} executed no compiler work")
        if fields["full"] + fields["tail"] != fields["runtime_requests"]:
            raise ValueError(f"tile {tile} has inconsistent full/tail accounting")
        physical_requests = (
            fields["runtime_requests"] + fields["validation_requests"]
        )
        physical_bytes = fields["runtime_bytes"] + fields["validation_bytes"]
        profile = profiles[tile]
        if int(profile.group("requests")) != physical_requests:
            raise ValueError(f"tile {tile} SST DMA request count disagrees with ABI")
        if int(profile.group("bytes")) != physical_bytes:
            raise ValueError(f"tile {tile} SST DMA byte count disagrees with ABI")
        expected_requests += physical_requests
        expected_bytes += physical_bytes
        full += fields["full"]
        tails += fields["tail"]
        if fields["validation_requests"] != 0:
            validation_tiles.append(tile)
        if passes[tile].group("case") != args.case:
            raise ValueError(f"tile {tile} reported a pass for the wrong case")

    if full == 0 or tails == 0:
        raise ValueError("case did not exercise both 4096-byte and tail DMA")
    if validation_tiles:
        raise ValueError(
            "post-run numerical output reads are outside the structural gate"
        )
    totals = statistic_totals(args.statistics)
    if totals["requests"] != expected_requests or totals["bytes"] != expected_bytes:
        raise ValueError(
            "global RAM totals disagree with generated ABI accounting: "
            f"requests={totals['requests']}/{expected_requests} "
            f"bytes={totals['bytes']}/{expected_bytes}"
        )
    print(
        f"materialized {args.case}: PASS (tiles={len(active)}, "
        f"full={full}, tail={tails}, dma_requests={expected_requests}, "
        f"dma_bytes={expected_bytes})"
    )


if __name__ == "__main__":
    main()
