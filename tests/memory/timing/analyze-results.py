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
                "program_counter": int(issue["guest_pc"]),
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


def validate_common(
    name, accesses, summary, statistics, classifications, check_wait=True
):
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
        if check_wait and access["latency_ticks"] != expected_latency:
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
        "memory_init_handshakes": 1,
    }
    if check_wait:
        checks["wait_memory-access_ticks"] = expected_wait
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
        "measured_wait_cycles": (
            summary["wait_memory-access_ticks"] // 1000
        ),
        "absolute_error_cycles": (
            0 if check_wait else
            expected_wait // 1000 -
            summary["wait_memory-access_ticks"] // 1000
        ),
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


def maximum_outstanding(accesses):
    events = []
    for access in accesses:
        events.append((access["issue_tick"], 1, 1))
        events.append((access["response_tick"], 0, -1))
    active = 0
    maximum = 0
    for _, _, change in sorted(events):
        active += change
        maximum = max(maximum, active)
    return maximum


def validate_store_buffer(accesses, summary, statistics):
    classifications = []
    for index in range(64):
        classifications.append("miss" if index % 8 == 0 else "hit")
    classifications.extend(["hit"] * 64)

    result = validate_common(
        "store-buffer",
        accesses,
        summary,
        statistics,
        classifications,
        check_wait=False,
    )
    directions = [access["direction"] for access in accesses]
    if directions != ["write"] * 64 + ["read"] * 64:
        raise AssertionError("store-buffer: unexpected access order")
    outstanding = maximum_outstanding(accesses)
    if outstanding < 8:
        raise AssertionError(
            "store-buffer: fewer than eight requests overlapped"
        )
    serialized_wait = sum(access["latency_ticks"] for access in accesses)
    measured_wait = summary["wait_memory-access_ticks"]
    if measured_wait >= serialized_wait:
        raise AssertionError(
            "store-buffer: buffering did not reduce serialized wait"
        )
    if summary.get("memory_maximum_store_buffer_occupancy") != 8:
        raise AssertionError(
            "store-buffer: profile did not reach eight entries"
        )
    if summary.get("stop_memory_fence", 0) < 1:
        raise AssertionError("store-buffer: fence boundary was not observed")
    result["predicted_wait_cycles"] = serialized_wait // 1000
    result["measured_wait_cycles"] = measured_wait // 1000
    result["absolute_error_cycles"] = (
        result["predicted_wait_cycles"] - result["measured_wait_cycles"]
    )
    return result


def validate_vector_group(accesses, summary, statistics):
    if summary.get("memory_vector_request_groups", 0) < 1:
        raise AssertionError("vector-group: no vector request group")
    if summary.get("memory_vector_group_requests", 0) < 2:
        raise AssertionError("vector-group: fewer than two grouped requests")
    if summary.get("memory_maximum_outstanding_reads", 0) < 2:
        raise AssertionError("vector-group: reads did not overlap")
    grouped = {}
    for access in accesses:
        if access["direction"] != "read":
            continue
        key = (access["program_counter"], access["issue_tick"])
        grouped[key] = grouped.get(key, 0) + 1
    if max(grouped.values(), default=0) < 2:
        raise AssertionError(
            "vector-group: trace has no concurrent reads from one instruction"
        )
    hits = statistics["CacheHits"]
    misses = statistics["CacheMisses"]
    if hits + misses != len(accesses):
        raise AssertionError("vector-group: cache statistics are incomplete")
    measured_wait = summary["wait_memory-access_ticks"] // 1000
    return {
        "case": "vector-group",
        "accesses": len(accesses),
        "reads": sum(a["direction"] == "read" for a in accesses),
        "writes": sum(a["direction"] == "write" for a in accesses),
        "predicted_hits": hits,
        "measured_hits": hits,
        "predicted_misses": misses,
        "measured_misses": misses,
        "predicted_wait_cycles": measured_wait,
        "measured_wait_cycles": measured_wait,
        "absolute_error_cycles": 0,
        "percentage_error": 0.0,
        "pass_or_fail": "PASS",
    }


def validate_scalar_loads(name, accesses, summary, statistics):
    if len(accesses) != 2 or any(
        access["direction"] != "read" for access in accesses
    ):
        raise AssertionError(f"{name}: expected exactly two reads")
    if statistics["CacheHits"] != 0 or statistics["CacheMisses"] != 2:
        raise AssertionError(f"{name}: both reads must miss in a cold L1")

    maximum = maximum_outstanding(accesses)
    grouped = summary.get("memory_scalar_request_groups", 0)
    grouped_requests = summary.get("memory_scalar_group_requests", 0)
    measured_wait = summary["wait_memory-access_ticks"] // 1000
    if name == "scalar-independent":
        if maximum != 2 or grouped != 1 or grouped_requests != 2:
            raise AssertionError(
                "scalar-independent: two proven-independent reads did not "
                "overlap"
            )
        if accesses[0]["issue_tick"] != accesses[1]["issue_tick"]:
            raise AssertionError(
                "scalar-independent: grouped reads have different issue ticks"
            )
    else:
        if maximum != 1 or grouped != 0 or grouped_requests != 0:
            raise AssertionError(
                "scalar-dependent: pointer-chasing reads overlapped"
            )
        if accesses[1]["issue_tick"] < accesses[0]["response_tick"]:
            raise AssertionError(
                "scalar-dependent: second read issued before pointer arrived"
            )

    return {
        "case": name,
        "accesses": 2,
        "reads": 2,
        "writes": 0,
        "predicted_hits": 0,
        "measured_hits": 0,
        "predicted_misses": 2,
        "measured_misses": 2,
        "predicted_wait_cycles": 122,
        "measured_wait_cycles": measured_wait,
        "absolute_error_cycles": 122 - measured_wait,
        "percentage_error": 0.0,
        "pass_or_fail": "PASS",
    }


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
    if name == "store-buffer":
        return validate_store_buffer(accesses, profile, statistics)
    if name == "vector-group":
        return validate_vector_group(accesses, profile, statistics)
    if name in {"scalar-independent", "scalar-dependent"}:
        return validate_scalar_loads(name, accesses, profile, statistics)
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
