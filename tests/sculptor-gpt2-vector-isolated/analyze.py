#!/usr/bin/env python3

import csv
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


TASK_ID = 152
EXPECTED_CHECKSUM = "0x00000000d8a0ef45"
EXPECTED_VECTOR_EVENTS = 9216
EXPECTED_VECTOR_BYTES = 36864
EXPECTED_VECTOR_MEMORY_INSTRUCTIONS = 1152

RESOURCE_PATTERN = re.compile(
    r"VECTOR_RESOURCE role=(?P<role>\w+) "
    r"slot=(?P<slot>0x[0-9a-f]+) "
    r"base=(?P<base>0x[0-9a-f]+) "
    r"bytes=(?P<bytes>0x[0-9a-f]+)"
)
CHECKSUM_PATTERN = re.compile(
    r"VECTOR_ISOLATED_PASS checksum=(0x[0-9a-f]+)"
)
INSTRUCTION_PATTERN = re.compile(
    r"^\s*([0-9a-f]+):\s+([a-zA-Z0-9_.]+)", re.MULTILINE
)


def read_summary(path):
    with path.open(encoding="utf-8", newline="") as source:
        return {
            row["metric"]: int(row["value"])
            for row in csv.DictReader(source)
            if row["metric"] != "tile_id"
        }


def read_task(path):
    with path.open(encoding="utf-8", newline="") as source:
        rows = [
            row for row in csv.DictReader(source)
            if int(row["task_id"]) == TASK_ID
        ]
    if [row["event"] for row in rows] != ["start", "finish"]:
        raise RuntimeError(f"task trace is incomplete: {path}")
    start, finish = rows
    return {
        "duration_ticks": (
            int(finish["sim_time_ticks"]) - int(start["sim_time_ticks"])
        ),
        "instructions": (
            int(finish["retired_instructions"])
            - int(start["retired_instructions"])
        ),
        "cpu_cycles": (
            int(finish["cpu_cycles"]) - int(start["cpu_cycles"])
        ),
    }


def read_resources(log_path):
    text = log_path.read_text(encoding="utf-8")
    checksum_match = CHECKSUM_PATTERN.search(text)
    if checksum_match is None:
        raise RuntimeError(f"simulation did not report success: {log_path}")
    resources = []
    for match in RESOURCE_PATTERN.finditer(text):
        resources.append(
            {
                "role": match.group("role"),
                "slot": int(match.group("slot"), 16),
                "base": int(match.group("base"), 16),
                "bytes": int(match.group("bytes"), 16),
            }
        )
    if len(resources) != 4:
        raise RuntimeError(f"expected four resource ranges: {log_path}")
    if any(resource["base"] % 32 != 0 for resource in resources):
        raise RuntimeError(f"resource base is not 32-byte aligned: {log_path}")
    return checksum_match.group(1), resources


def read_memory(path, resources):
    rows = []
    with path.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            if row["event"] == "response" and int(row["task_id"]) == TASK_ID:
                rows.append(row)

    ranges = Counter()
    range_bytes = Counter()
    for row in rows:
        address = int(row["address"])
        size = int(row["size"])
        for resource in resources:
            base = resource["base"]
            if base <= address and address + size <= base + resource["bytes"]:
                key = (
                    resource["role"],
                    resource["slot"],
                    row["direction"],
                )
                ranges[key] += 1
                range_bytes[key] += size
                break

    return {
        "rows": rows,
        "events": len(rows),
        "bytes": sum(int(row["size"]) for row in rows),
        "stall_ticks": sum(int(row["latency_ticks"]) for row in rows),
        "reads": sum(row["direction"] == "read" for row in rows),
        "writes": sum(row["direction"] == "write" for row in rows),
        "size_histogram": dict(sorted(Counter(
            int(row["size"]) for row in rows
        ).items())),
        "latency_histogram": dict(sorted(Counter(
            int(row["latency_ticks"]) for row in rows
        ).items())),
        "resource_events": sum(ranges.values()),
        "resource_bytes": sum(range_bytes.values()),
        "resource_ranges": [
            {
                "role": key[0],
                "slot": key[1],
                "direction": key[2],
                "events": ranges[key],
                "bytes": range_bytes[key],
            }
            for key in sorted(ranges)
        ],
    }


def vector_memory_pcs(objdump, elf):
    output = subprocess.run(
        [
            str(objdump),
            "-d",
            "--no-show-raw-insn",
            "--disassemble-symbols=__golem_tile_execute_task_152",
            str(elf),
        ],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    return {
        int(address, 16)
        for address, mnemonic in INSTRUCTION_PATTERN.findall(output)
        if mnemonic in {"vle32.v", "vse32.v"}
    }


def analyze_trial(root, name):
    trial = root / name
    checksum, resources = read_resources(trial / "simulation.log")
    task = read_task(trial / "profile" / "tile-0.csv")
    memory = read_memory(trial / "profile" / "tile-0-memory.csv", resources)
    summary = read_summary(trial / "profile" / "tile-0-summary.csv")
    return {
        "checksum": checksum,
        "resources": resources,
        "task": task,
        "memory": memory,
        "summary": summary,
    }


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: analyze.py OUTPUT_ROOT LLVM_OBJDUMP RESULT_JSON")
    root = Path(sys.argv[1])
    objdump = Path(sys.argv[2])
    result_path = Path(sys.argv[3])

    baseline = analyze_trial(root, "blocking-baseline-v2")
    candidate = analyze_trial(root, "blocking-candidate-v2")
    if baseline["checksum"] != EXPECTED_CHECKSUM:
        raise RuntimeError("baseline checksum changed")
    if candidate["checksum"] != baseline["checksum"]:
        raise RuntimeError("candidate checksum differs from baseline")

    baseline_memory = baseline["memory"]
    candidate_memory = candidate["memory"]
    for field in (
        "events", "bytes", "stall_ticks", "reads", "writes",
        "size_histogram", "latency_histogram", "resource_events",
        "resource_bytes", "resource_ranges",
    ):
        if candidate_memory[field] != baseline_memory[field]:
            raise RuntimeError(f"memory field changed: {field}")

    pcs = vector_memory_pcs(objdump, root / "candidate.elf")
    pc_counts = Counter(
        int(row["guest_pc"])
        for row in candidate_memory["rows"]
        if int(row["guest_pc"]) in pcs
    )
    vector_events = sum(pc_counts.values())
    vector_bytes = sum(
        int(row["size"])
        for row in candidate_memory["rows"]
        if int(row["guest_pc"]) in pcs
    )
    if vector_events != EXPECTED_VECTOR_EVENTS:
        raise RuntimeError(f"expected 9216 vector events, found {vector_events}")
    if vector_bytes != EXPECTED_VECTOR_BYTES:
        raise RuntimeError(f"expected 36864 vector bytes, found {vector_bytes}")
    if any(count % 8 != 0 for count in pc_counts.values()):
        raise RuntimeError("a vector memory site did not expand by eight")
    vector_memory_instructions = sum(count // 8 for count in pc_counts.values())
    if vector_memory_instructions != EXPECTED_VECTOR_MEMORY_INSTRUCTIONS:
        raise RuntimeError("vector memory instruction count changed")

    task_time_reduction = (
        baseline["task"]["duration_ticks"]
        - candidate["task"]["duration_ticks"]
    )
    instruction_reduction = (
        baseline["task"]["instructions"]
        - candidate["task"]["instructions"]
    )
    result = {
        "baseline": baseline,
        "candidate": candidate,
        "comparison": {
            "task_time_reduction_ticks": task_time_reduction,
            "task_time_reduction_percent": (
                100.0 * task_time_reduction
                / baseline["task"]["duration_ticks"]
            ),
            "speedup": (
                baseline["task"]["duration_ticks"]
                / candidate["task"]["duration_ticks"]
            ),
            "instruction_reduction": instruction_reduction,
            "instruction_reduction_percent": (
                100.0 * instruction_reduction
                / baseline["task"]["instructions"]
            ),
            "cpu_cycle_reduction": (
                baseline["task"]["cpu_cycles"]
                - candidate["task"]["cpu_cycles"]
            ),
            "vector_memory_instructions": vector_memory_instructions,
            "vector_memory_events": vector_events,
            "vector_memory_bytes": vector_bytes,
            "total_vector_instruction_delta": (
                candidate["summary"]["vector_instructions"]
                - baseline["summary"]["vector_instructions"]
            ),
        },
    }
    del result["baseline"]["memory"]["rows"]
    del result["candidate"]["memory"]["rows"]
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["comparison"], indent=2))


if __name__ == "__main__":
    main()
