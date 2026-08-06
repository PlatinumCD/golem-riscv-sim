#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path


SCALAR_BODY_INSTRUCTIONS = 1024
VECTOR_BODY_INSTRUCTIONS = 1027
VECTOR_REGION_EXTRA_SCALAR_INSTRUCTIONS = 2
SST_TICKS_PER_CPU_CYCLE = 1000


def read_summary(path):
    with path.open(newline="") as stream:
        return {
            row["metric"]: int(row["value"])
            for row in csv.DictReader(stream)
        }


def read_tasks(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    active = {}
    tasks = {}
    for row in rows:
        task_id = int(row["task_id"])
        if row["event"] == "start":
            if task_id in active:
                raise AssertionError(f"duplicate task start {task_id}")
            active[task_id] = row
            continue
        start = active.pop(task_id, None)
        if start is None:
            raise AssertionError(f"task {task_id} finished without starting")
        tasks[task_id] = {
            "instructions": (
                int(row["retired_instructions"])
                - int(start["retired_instructions"])
            ),
            "cycles": int(row["cpu_cycles"]) - int(start["cpu_cycles"]),
            "ticks": (
                int(row["sim_time_ticks"])
                - int(start["sim_time_ticks"])
            ),
        }
    if active:
        raise AssertionError(f"unfinished tasks: {sorted(active)}")
    if set(tasks) != {0, 1, 2}:
        raise AssertionError(f"unexpected task set: {sorted(tasks)}")
    return tasks


def expected_cycles(instructions, vectors, width):
    return max((instructions + width - 1) // width, vectors)


def parse_trial(specification):
    width, quantum, summary, tasks = specification.split(":", 3)
    return (
        int(width),
        int(quantum),
        read_summary(Path(summary)),
        read_tasks(Path(tasks)),
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "trials",
        nargs="+",
        help="ISSUE_WIDTH:QUANTUM:SUMMARY_CSV:TASK_CSV",
    )
    arguments = parser.parse_args()

    trials = [parse_trial(trial) for trial in arguments.trials]
    reference_counts = None
    per_width = {}
    results = []

    for width, quantum, summary, tasks in trials:
        if summary["vector_instructions"] != VECTOR_BODY_INSTRUCTIONS:
            raise AssertionError(
                f"width {width}, quantum {quantum}: expected exactly "
                f"{VECTOR_BODY_INSTRUCTIONS} vector instructions, observed "
                f"{summary['vector_instructions']}"
            )

        baseline = tasks[0]["instructions"]
        if tasks[1]["instructions"] - baseline != SCALAR_BODY_INSTRUCTIONS:
            raise AssertionError(
                f"width {width}, quantum {quantum}: scalar region differs "
                f"from baseline by "
                f"{tasks[1]['instructions'] - baseline}, expected "
                f"{SCALAR_BODY_INSTRUCTIONS}"
            )
        expected_vector_region_delta = (
            VECTOR_BODY_INSTRUCTIONS
            + VECTOR_REGION_EXTRA_SCALAR_INSTRUCTIONS
        )
        if tasks[2]["instructions"] - baseline != expected_vector_region_delta:
            raise AssertionError(
                f"width {width}, quantum {quantum}: vector region differs "
                f"from baseline by "
                f"{tasks[2]['instructions'] - baseline}, expected "
                f"{expected_vector_region_delta}"
            )

        counts = (
            summary["instructions"],
            summary["vector_instructions"],
            tasks[0]["instructions"],
            tasks[1]["instructions"],
            tasks[2]["instructions"],
        )
        if reference_counts is None:
            reference_counts = counts
        elif counts != reference_counts:
            raise AssertionError(
                f"retired counts changed with configuration: "
                f"{counts} versus {reference_counts}"
            )

        vector_counts = {0: 0, 1: 0, 2: VECTOR_BODY_INSTRUCTIONS}
        labels = {0: "baseline", 1: "scalar-1024", 2: "rvv-1027"}
        for task_id in (0, 1, 2):
            measured = tasks[task_id]["cycles"]
            predicted = expected_cycles(
                tasks[task_id]["instructions"],
                vector_counts[task_id],
                width,
            )
            if measured != predicted:
                raise AssertionError(
                    f"width {width}, quantum {quantum}, "
                    f"{labels[task_id]}: predicted {predicted} cycles, "
                    f"observed {measured}"
                )
            if tasks[task_id]["ticks"] != (
                measured * SST_TICKS_PER_CPU_CYCLE
            ):
                raise AssertionError(
                    f"width {width}, quantum {quantum}, "
                    f"{labels[task_id]}: {measured} cycles covered "
                    f"{tasks[task_id]['ticks']} SST ticks"
                )
            results.append(
                {
                    "issue_width": width,
                    "instruction_quantum": quantum,
                    "region": labels[task_id],
                    "retired_instructions": tasks[task_id]["instructions"],
                    "retired_vector_instructions": vector_counts[task_id],
                    "predicted_cycles": predicted,
                    "measured_cycles": measured,
                    "absolute_error": abs(measured - predicted),
                    "percentage_error": 0.0,
                    "pass_or_fail": "PASS",
                }
            )

        invariance = (
            summary["instructions"],
            summary["vector_instructions"],
            summary["cpu_cycles"],
            summary["finish_tick"],
            tuple(
                (
                    task_id,
                    tasks[task_id]["instructions"],
                    tasks[task_id]["cycles"],
                    tasks[task_id]["ticks"],
                )
                for task_id in (0, 1, 2)
            ),
        )
        previous = per_width.setdefault(width, invariance)
        if previous != invariance:
            raise AssertionError(
                f"issue width {width} changed with instruction quantum: "
                f"{invariance} versus {previous}"
            )

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    print(
        "width  quantum  region        instructions  vectors  "
        "predicted  measured  error"
    )
    for row in results:
        print(
            f"{row['issue_width']:>5} "
            f"{row['instruction_quantum']:>8} "
            f"{row['region']:<13} "
            f"{row['retired_instructions']:>12} "
            f"{row['retired_vector_instructions']:>8} "
            f"{row['predicted_cycles']:>10} "
            f"{row['measured_cycles']:>9} "
            f"{row['absolute_error']:>6}"
        )


if __name__ == "__main__":
    main()
