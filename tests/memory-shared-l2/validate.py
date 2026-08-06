#!/usr/bin/env python3

import csv
import json
import sys
from pathlib import Path


GUEST_BASE = 0x80000000
TILE_STRIDE = 16 * 1024 * 1024
NO_TASK = (1 << 32) - 1


def memory_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def validate_tile(profile, tile):
    path = profile / f"tile-{tile}-memory.csv"
    if not path.is_file():
        raise AssertionError(f"missing memory trace {path}")
    rows = memory_rows(path)
    issues = [row for row in rows if row["event"] == "issue"]
    responses = [row for row in rows if row["event"] == "response"]
    if not issues or len(issues) != len(responses):
        raise AssertionError(
            f"tile {tile} has unbalanced memory requests"
        )
    response_by_request = {
        int(row["request_id"]): row for row in responses
    }
    accesses = []
    for row in issues:
        guest = int(row["address"])
        timing = int(row["timing_address"])
        expected = tile * TILE_STRIDE + guest - GUEST_BASE
        if timing != expected:
            raise AssertionError(
                f"tile {tile} translated {guest:#x} to {timing:#x}, "
                f"expected {expected:#x}"
            )
        response = response_by_request.get(int(row["request_id"]))
        if response is None:
            raise AssertionError(
                f"tile {tile} request {row['request_id']} has no response"
            )
        accesses.append(
            {
                "tile": tile,
                "timing_address": timing,
                "bank": (timing // 64) % 2,
                "issue": int(row["issue_tick"]),
                "response": int(response["event_tick"]),
                "task_id": int(row["task_id"]),
                "direction": row["direction"],
                "size": int(row["size"]),
            }
        )
    attributed = [
        access for access in accesses if access["task_id"] != NO_TASK
    ]
    if not attributed:
        raise AssertionError(f"tile {tile} has no task-attributed access")
    required_tasks = {11 if tile == 0 else 12}
    observed_tasks = {access["task_id"] for access in attributed}
    if not required_tasks.issubset(observed_tasks):
        raise AssertionError(
            f"tile {tile} is missing task attribution for "
            f"{sorted(required_tasks - observed_tasks)}"
        )
    return accesses


def validate_statistics(path):
    counters = {}
    with path.open(newline="") as stream:
        for row in csv.reader(stream):
            if len(row) < 7 or row[1] not in ("CacheHits", "CacheMisses"):
                continue
            counters[(row[0], row[1])] = int(row[6])
    for tile in range(2):
        component = f"tile{tile}.private_l1"
        if counters.get((component, "CacheHits"), 0) <= 0:
            raise AssertionError(f"{component} did not record an L1 hit")
        if counters.get((component, "CacheMisses"), 0) <= 0:
            raise AssertionError(f"{component} did not record an L1 miss")
    l2_hits = sum(
        counters.get((f"shared_l2.bank{bank}", "CacheHits"), 0)
        for bank in range(2)
    )
    l2_misses = sum(
        counters.get((f"shared_l2.bank{bank}", "CacheMisses"), 0)
        for bank in range(2)
    )
    if l2_hits <= 0:
        raise AssertionError("no L1 miss was served by the shared L2")
    if l2_misses <= 0:
        raise AssertionError("no L2 miss reached lower memory")
    return counters, l2_hits, l2_misses


def find_same_bank_overlap(first, second):
    for left in first:
        for right in second:
            if left["bank"] != right["bank"]:
                continue
            if max(left["issue"], right["issue"]) < min(
                left["response"], right["response"]
            ):
                return left, right
    raise AssertionError("the two tiles produced no same-bank overlap")


def task_totals(accesses):
    totals = {}
    for access in accesses:
        if access["task_id"] == NO_TASK:
            continue
        key = (access["tile"], access["task_id"])
        total = totals.setdefault(
            key,
            {
                "requests": 0,
                "reads": 0,
                "writes": 0,
                "bytes": 0,
                "stall_ticks": 0,
            },
        )
        total["requests"] += 1
        total["reads"] += access["direction"] == "read"
        total["writes"] += access["direction"] == "write"
        total["bytes"] += access["size"]
        total["stall_ticks"] += access["response"] - access["issue"]
    return totals


def validate_dma(profile):
    path = profile / "tile-1-receive-dma.csv"
    with path.open(newline="") as stream:
        completions = [
            row for row in csv.DictReader(stream)
            if row["event"] == "complete"
        ]
    if len(completions) != 1:
        raise AssertionError("expected one receive-DMA completion")
    lines = int(completions[0]["invalidation_lines"])
    if lines <= 0:
        raise AssertionError("receive DMA did not invalidate destination L1")
    return lines


def main():
    profile = Path(sys.argv[1])
    statistics = Path(sys.argv[2])
    tile0 = validate_tile(profile, 0)
    tile1 = validate_tile(profile, 1)
    if max(int(row["timing_address"]) for row in tile0) >= TILE_STRIDE:
        raise AssertionError("tile 0 escaped its timing-address namespace")
    if min(int(row["timing_address"]) for row in tile1) < TILE_STRIDE:
        raise AssertionError("tile 1 aliased tile 0 timing addresses")
    counters, l2_hits, l2_misses = validate_statistics(statistics)
    left, right = find_same_bank_overlap(tile0, tile1)
    invalidation_lines = validate_dma(profile)
    totals = task_totals(tile0 + tile1)
    evidence = {
        "l1": {
            f"tile{tile}": {
                "hits": counters[(f"tile{tile}.private_l1", "CacheHits")],
                "misses": counters[
                    (f"tile{tile}.private_l1", "CacheMisses")
                ],
            }
            for tile in range(2)
        },
        "shared_l2_hits": l2_hits,
        "shared_l2_misses_to_lower_memory": l2_misses,
        "same_bank_overlap": {
            "bank": left["bank"],
            "tile0_request": [left["issue"], left["response"]],
            "tile1_request": [right["issue"], right["response"]],
        },
        "dma_invalidation_lines": invalidation_lines,
        "task_totals": {
            f"tile{tile}.task{task}": values
            for (tile, task), values in totals.items()
        },
    }
    (profile.parent / "evidence.json").write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n"
    )
    print(
        f"shared-L2 fixture: tile0={len(tile0)} accesses, "
        f"tile1={len(tile1)} accesses, L2 hits={l2_hits}, "
        f"lower-memory misses={l2_misses}, "
        f"same-bank overlap=bank{left['bank']}, "
        f"DMA invalidations={invalidation_lines} lines"
    )


if __name__ == "__main__":
    main()
