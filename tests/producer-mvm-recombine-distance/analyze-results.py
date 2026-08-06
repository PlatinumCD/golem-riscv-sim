#!/usr/bin/env python3
"""Summarize producer-MVM-recombine task, network, DMA, and analog timing."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


PRODUCER_TASK = 1000
MVM_TASK = 2000
RECOMBINE_TASK = 3000
ACTIVATION_ROUTE = 100
PARTIAL_ROUTES = {200, 201}
TICKS_PER_NS = 1000


def rows(paths: list[Path]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for path in sorted(paths):
        with path.open(encoding="utf-8", newline="") as source:
            result.extend(csv.DictReader(source))
    return result


def task_intervals(directory: Path) -> dict[int, dict[str, int]]:
    active: dict[int, dict[str, str]] = {}
    intervals: dict[int, dict[str, int]] = {}
    for row in sorted(
        rows(list(directory.glob("tile-*.csv"))),
        key=lambda item: (
            int(item["sim_time_ticks"]),
            0 if item["event"] == "start" else 1,
        ),
    ):
        task_id = int(row["task_id"])
        if row["event"] == "start":
            active[task_id] = row
            continue
        start = active.pop(task_id)
        intervals[task_id] = {
            "tile": int(row["tile_id"]),
            "start": int(start["sim_time_ticks"]),
            "finish": int(row["sim_time_ticks"]),
            "instructions": (
                int(row["retired_instructions"]) -
                int(start["retired_instructions"])
            ),
            "cycles": int(row["cpu_cycles"]) - int(start["cpu_cycles"]),
        }
    if active:
        raise RuntimeError(f"unfinished task markers: {sorted(active)}")
    expected = {PRODUCER_TASK, MVM_TASK, RECOMBINE_TASK}
    if set(intervals) != expected:
        raise RuntimeError(
            f"expected task IDs {sorted(expected)}, "
            f"observed {sorted(intervals)}"
        )
    return intervals


def analyze_trial(trial: dict[str, str]) -> dict[str, object]:
    run_directory = Path(trial["run_directory"])
    tasks = task_intervals(run_directory / "tasks")
    producer = tasks[PRODUCER_TASK]
    mvm = tasks[MVM_TASK]
    recombine = tasks[RECOMBINE_TASK]
    if not (
        producer["start"] <= producer["finish"] <= mvm["start"] <=
        mvm["finish"] <= recombine["start"] <= recombine["finish"]
    ):
        raise RuntimeError(f"{trial['name']}: invalid task order")

    network = rows(list((run_directory / "profile").glob(
        "tile-*-network.csv"
    )))
    injected = [
        row for row in network
        if row["event"] == "inject" and int(row["route_id"]) in {
            ACTIVATION_ROUTE, *PARTIAL_ROUTES
        }
    ]
    payload = [row for row in injected if row["kind"] == "frame-payload"]
    headers = [row for row in injected if row["kind"] == "frame-header"]
    data_words = sum(int(row["payload_words"]) for row in payload)
    data_word_hops = sum(int(row["word_hops"]) for row in payload)
    protocol_words = sum(int(row["protocol_words"]) for row in headers)
    protocol_word_hops = sum(int(row["word_hops"]) for row in headers)
    endpoint_queue_ticks = sum(
        int(row["endpoint_queue_ticks"]) for row in injected
    )

    input_distance = int(trial["input_distance"])
    output_distance = int(trial["output_distance"])
    expected_words = (
        (512 if input_distance else 0) +
        (512 if output_distance else 0)
    )
    expected_word_hops = (
        512 * input_distance + 512 * output_distance
    )
    expected_protocol_words = (
        (5 if input_distance else 0) +
        (10 if output_distance else 0)
    )
    if (
        data_words != expected_words or
        data_word_hops != expected_word_hops or
        protocol_words != expected_protocol_words
    ):
        raise RuntimeError(
            f"{trial['name']}: unexpected network accounting: "
            f"data={data_words}/{expected_words}, "
            f"word_hops={data_word_hops}/{expected_word_hops}, "
            f"protocol={protocol_words}/{expected_protocol_words}"
        )

    dma = rows(list((run_directory / "profile").glob(
        "tile-*-receive-dma.csv"
    )))
    route_ids = {ACTIVATION_ROUTE, *PARTIAL_ROUTES}
    dma_completions = [
        row for row in dma
        if row["event"] == "complete" and int(row["route_id"]) in route_ids
    ]
    dma_cycles = sum(int(row["service_cycles"]) for row in dma_completions)

    analog = rows(list((run_directory / "profile").glob(
        "tile-*-analog.csv"
    )))
    measured_analog = [
        row for row in analog
        if mvm["start"] <= int(row["event_tick"]) <= mvm["finish"]
    ]
    submitted = [row for row in measured_analog if row["phase"] == "submitted"]
    completed = [row for row in measured_analog if row["phase"] == "complete"]
    if len(submitted) != 6 or len(completed) != 6:
        raise RuntimeError(
            f"{trial['name']}: expected six measured analog commands, "
            f"observed {len(submitted)} submissions and "
            f"{len(completed)} completions"
        )
    analog_span = (
        max(int(row["event_tick"]) for row in completed) -
        min(int(row["event_tick"]) for row in submitted)
    )

    total_ticks = recombine["finish"] - producer["start"]
    producer_ticks = producer["finish"] - producer["start"]
    activation_boundary = mvm["start"] - producer["finish"]
    mvm_ticks = mvm["finish"] - mvm["start"]
    partial_boundary = recombine["start"] - mvm["finish"]
    recombine_ticks = recombine["finish"] - recombine["start"]

    return {
        "name": trial["name"],
        "experiment": trial["experiment"],
        "input_distance": input_distance,
        "output_distance": output_distance,
        "producer_tile": trial["producer_tile"],
        "mvm_tile": trial["mvm_tile"],
        "recombine_tile": trial["recombine_tile"],
        "total_ns": f"{total_ticks / TICKS_PER_NS:.3f}",
        "producer_ns": f"{producer_ticks / TICKS_PER_NS:.3f}",
        "activation_boundary_ns": (
            f"{activation_boundary / TICKS_PER_NS:.3f}"
        ),
        "mvm_ns": f"{mvm_ticks / TICKS_PER_NS:.3f}",
        "partial_boundary_ns": f"{partial_boundary / TICKS_PER_NS:.3f}",
        "recombine_ns": f"{recombine_ticks / TICKS_PER_NS:.3f}",
        "analog_service_ns": f"{analog_span / TICKS_PER_NS:.3f}",
        "producer_instructions": producer["instructions"],
        "mvm_instructions": mvm["instructions"],
        "recombine_instructions": recombine["instructions"],
        "data_words": data_words,
        "data_word_hops": data_word_hops,
        "protocol_words": protocol_words,
        "protocol_word_hops": protocol_word_hops,
        "endpoint_queue_ns": (
            f"{endpoint_queue_ticks / TICKS_PER_NS:.3f}"
        ),
        "rx_dma_cycles": dma_cycles,
        "pass": 1,
    }


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: analyze-results.py RUN_MANIFEST.tsv RESULTS.csv"
        )
    manifest = Path(sys.argv[1])
    output = Path(sys.argv[2])
    with manifest.open(encoding="utf-8", newline="") as source:
        trials = list(csv.DictReader(source, delimiter="\t"))
    results = [analyze_trial(trial) for trial in trials]
    if not results:
        raise RuntimeError("run manifest contains no trials")

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)

    print(
        "configuration                         din dout   total(ns) "
        "input-gap(ns) output-gap(ns) word-hops"
    )
    for result in results:
        print(
            f"{result['name']:<36} "
            f"{result['input_distance']:>3} "
            f"{result['output_distance']:>4} "
            f"{result['total_ns']:>11} "
            f"{result['activation_boundary_ns']:>13} "
            f"{result['partial_boundary_ns']:>14} "
            f"{result['data_word_hops']:>9}"
        )
    print(f"producer-MVM-recombine distance: {len(results)}/{len(results)} passed")
    print(output)


if __name__ == "__main__":
    main()
