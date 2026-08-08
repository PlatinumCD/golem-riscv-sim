#!/usr/bin/env python3

import argparse
import csv
import re
from pathlib import Path


HIT_TICKS = 5000
MISS_TICKS = 61000
LINE_BYTES = 64
SET_COUNT = 128
SET_SPAN_BYTES = LINE_BYTES * SET_COUNT

STATISTIC = re.compile(
    r"tile0\.private_l1\.(CacheHits|CacheMisses).*"
    r"Sum\.u64 = ([0-9]+)"
)


def read_summary(path):
    with path.open(newline="") as stream:
        return {
            row["metric"]: int(row["value"])
            for row in csv.DictReader(stream)
        }


def read_accesses(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    issued = {}
    accesses = []
    for row in rows:
        request = int(row["request_id"])
        if row["event"] == "issue":
            issued[request] = row
            continue
        issue = issued.pop(request, None)
        if issue is None:
            raise AssertionError(f"response without issue for request {request}")
        accesses.append(
            {
                "request_id": request,
                "address": int(issue["address"]),
                "size": int(issue["size"]),
                "direction": issue["direction"],
                "issue_tick": int(issue["issue_tick"]),
                "response_tick": int(row["event_tick"]),
                "latency_ticks": int(row["event_tick"])
                - int(issue["issue_tick"]),
            }
        )
    if issued:
        raise AssertionError(f"requests without responses: {sorted(issued)}")
    accesses.sort(key=lambda access: access["issue_tick"])
    return accesses


def read_statistics(path):
    statistics = {}
    text = path.read_text()
    for match in STATISTIC.finditer(text):
        statistics[match.group(1)] = int(match.group(2))
    if set(statistics) != {"CacheHits", "CacheMisses"}:
        raise AssertionError(f"missing cache statistics in {path}")
    profile_lines = [
        line for line in text.splitlines()
        if "MITTENS_PROFILE tile=0 " in line
    ]
    if len(profile_lines) != 1:
        raise AssertionError(f"missing Mittens profile counters in {path}")
    counters = {
        name: int(value)
        for name, value in re.findall(
            r"([a-z0-9_]+)=([0-9]+)", profile_lines[0]
        )
    }
    return statistics, counters


def validate_common(name, accesses, summary, statistics, classifications):
    if len(accesses) != len(classifications):
        raise AssertionError(
            f"{name}: expected {len(classifications)} accesses, "
            f"observed {len(accesses)}"
        )

    expected_hits = classifications.count("hit")
    expected_misses = classifications.count("miss")
    if statistics["CacheHits"] != expected_hits:
        raise AssertionError(
            f"{name}: expected {expected_hits} cache hits, "
            f"observed {statistics['CacheHits']}"
        )
    if statistics["CacheMisses"] != expected_misses:
        raise AssertionError(
            f"{name}: expected {expected_misses} cache misses, "
            f"observed {statistics['CacheMisses']}"
        )

    for index, (access, classification) in enumerate(
        zip(accesses, classifications)
    ):
        expected_latency = HIT_TICKS if classification == "hit" else MISS_TICKS
        if access["latency_ticks"] != expected_latency:
            raise AssertionError(
                f"{name}: access {index} expected {classification} latency "
                f"{expected_latency}, observed {access['latency_ticks']}"
            )
        if access["size"] != 8:
            raise AssertionError(
                f"{name}: access {index} has size {access['size']}, expected 8"
            )

    expected_wait = expected_hits * HIT_TICKS + expected_misses * MISS_TICKS
    checks = {
        "memory_requests": len(accesses),
        "memory_responses": len(accesses),
        "memory_reads": sum(
            access["direction"] == "read" for access in accesses
        ),
        "memory_writes": sum(
            access["direction"] == "write" for access in accesses
        ),
        "wait_memory-access_ticks": expected_wait,
        "memory_init_handshakes": 1,
    }
    for metric, expected in checks.items():
        if summary.get(metric) != expected:
            raise AssertionError(
                f"{name}: summary {metric} expected {expected}, "
                f"observed {summary.get(metric)}"
            )

    return {
        "case": name,
        "accesses": len(accesses),
        "reads": checks["memory_reads"],
        "writes": checks["memory_writes"],
        "predicted_hits": expected_hits,
        "measured_hits": statistics["CacheHits"],
        "predicted_misses": expected_misses,
        "measured_misses": statistics["CacheMisses"],
        "predicted_wait_cycles": expected_wait // 1000,
        "measured_wait_cycles": summary["wait_memory-access_ticks"] // 1000,
        "absolute_error_cycles": 0,
        "percentage_error": 0.0,
        "pass_or_fail": "PASS",
    }


def validate_conflict(accesses, summary, statistics):
    classifications = [
        "miss", "hit", "hit", "miss", "miss",
        "miss", "hit", "miss", "miss", "hit",
    ]
    base = accesses[0]["address"]
    expected_offsets = [
        0,
        0,
        0,
        SET_SPAN_BYTES,
        2 * SET_SPAN_BYTES,
        3 * SET_SPAN_BYTES,
        0,
        4 * SET_SPAN_BYTES,
        SET_SPAN_BYTES,
        0,
    ]
    offsets = [access["address"] - base for access in accesses]
    if offsets != expected_offsets:
        raise AssertionError(
            f"conflict: address offsets differ: {offsets}"
        )
    sets = {
        (access["address"] // LINE_BYTES) % SET_COUNT
        for access in accesses
    }
    if len(sets) != 1:
        raise AssertionError(
            f"conflict: accesses map to multiple sets: {sorted(sets)}"
        )
    directions = [access["direction"] for access in accesses]
    if directions != [
        "read", "read", "write", "read", "read",
        "read", "read", "read", "read", "read",
    ]:
        raise AssertionError(f"conflict: directions differ: {directions}")
    return validate_common(
        "conflict", accesses, summary, statistics, classifications
    )


def validate_capacity(accesses, summary, statistics):
    classifications = ["miss"] * 512 + ["hit", "miss", "miss"]
    base = accesses[0]["address"]
    expected_addresses = [
        base + index * LINE_BYTES for index in range(512)
    ] + [
        base,
        base + 512 * LINE_BYTES,
        base + 128 * LINE_BYTES,
    ]
    addresses = [access["address"] for access in accesses]
    if addresses != expected_addresses:
        mismatch = next(
            (
                index
                for index, (actual, expected) in enumerate(
                    zip(addresses, expected_addresses)
                )
                if actual != expected
            ),
            min(len(addresses), len(expected_addresses)),
        )
        raise AssertionError(
            f"capacity: address sequence differs at access {mismatch}"
        )
    if any(access["direction"] != "read" for access in accesses):
        raise AssertionError("capacity: expected read-only traffic")
    return validate_common(
        "capacity", accesses, summary, statistics, classifications
    )


def parse_case(specification):
    name, memory, summary, log = specification.split(":", 3)
    accesses = read_accesses(Path(memory))
    profile = read_summary(Path(summary))
    statistics, counters = read_statistics(Path(log))
    profile.update(counters)
    if name == "conflict":
        return validate_conflict(accesses, profile, statistics)
    if name == "capacity":
        return validate_capacity(accesses, profile, statistics)
    raise AssertionError(f"unsupported case {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "cases",
        nargs="+",
        help="NAME:MEMORY_CSV:SUMMARY_CSV:SIMULATION_LOG",
    )
    arguments = parser.parse_args()
    results = [parse_case(case) for case in arguments.cases]

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    print(
        "case      accesses  reads  writes  hits  misses  "
        "predicted cycles  measured cycles  error"
    )
    for result in results:
        print(
            f"{result['case']:<9} "
            f"{result['accesses']:>8} "
            f"{result['reads']:>6} "
            f"{result['writes']:>7} "
            f"{result['measured_hits']:>5} "
            f"{result['measured_misses']:>7} "
            f"{result['predicted_wait_cycles']:>16} "
            f"{result['measured_wait_cycles']:>16} "
            f"{result['absolute_error_cycles']:>6}"
        )


if __name__ == "__main__":
    main()
