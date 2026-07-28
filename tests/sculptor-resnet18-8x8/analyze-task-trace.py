#!/usr/bin/env python3

import argparse
import csv
import re
from pathlib import Path


TRACE_PATTERN = re.compile(
    r"MITTENS_TASK_TRACE "
    r"sim_time_ticks=(?P<time>[0-9]+) "
    r"event=(?P<event>start|finish) "
    r"tile=(?P<tile>[0-9]+) "
    r"task=(?P<task>[0-9]+) "
    r"execution=(?P<execution>[0-9]+)"
)


def parse_log_trace(log_path):
    records = []
    for line_number, line in enumerate(
        log_path.read_text(encoding="utf-8", errors="replace").splitlines(),
        start=1,
    ):
        match = TRACE_PATTERN.search(line)
        if match is None:
            continue
        records.append(
            {
                "sim_time_ps": int(match.group("time")),
                "event": match.group("event"),
                "tile_id": int(match.group("tile")),
                "task_id": int(match.group("task")),
                "execution_id": int(match.group("execution")),
                "line_number": line_number,
            }
        )
    records.sort(key=lambda record: (
        record["sim_time_ps"],
        record["line_number"],
    ))
    return records


def parse_csv_trace(directory):
    records = []
    sequence = 0
    for trace_path in sorted(directory.glob("tile-*.csv")):
        with trace_path.open(
            "r", encoding="utf-8", newline=""
        ) as trace_file:
            reader = csv.DictReader(trace_file)
            expected = {
                "sim_time_ticks",
                "event",
                "tile_id",
                "task_id",
                "execution_id",
            }
            if set(reader.fieldnames or []) != expected:
                raise RuntimeError(
                    f"{trace_path} has an invalid task trace header"
                )
            for row in reader:
                sequence += 1
                if row["event"] not in {"start", "finish"}:
                    raise RuntimeError(
                        f"{trace_path} has invalid event {row['event']}"
                    )
                records.append(
                    {
                        "sim_time_ps": int(row["sim_time_ticks"]),
                        "event": row["event"],
                        "tile_id": int(row["tile_id"]),
                        "task_id": int(row["task_id"]),
                        "execution_id": int(row["execution_id"]),
                        "line_number": sequence,
                    }
                )
    records.sort(key=lambda record: (
        record["sim_time_ps"],
        record["line_number"],
    ))
    return records


def parse_trace(source):
    if source.is_dir():
        return parse_csv_trace(source)
    return parse_log_trace(source)


def pair_tasks(records):
    active = {}
    completed = []
    for record in records:
        key = (
            record["tile_id"],
            record["task_id"],
            record["execution_id"],
        )
        if record["event"] == "start":
            if key in active:
                raise RuntimeError(f"duplicate task start for {key}")
            active[key] = record
            continue
        start = active.pop(key, None)
        if start is None:
            raise RuntimeError(f"task finish without start for {key}")
        if record["sim_time_ps"] < start["sim_time_ps"]:
            raise RuntimeError(f"negative task duration for {key}")
        completed.append(
            {
                "tile_id": record["tile_id"],
                "task_id": record["task_id"],
                "execution_id": record["execution_id"],
                "start_ps": start["sim_time_ps"],
                "finish_ps": record["sim_time_ps"],
                "duration_ps": (
                    record["sim_time_ps"] - start["sim_time_ps"]
                ),
            }
        )
    if active:
        raise RuntimeError(
            "task starts without finishes: "
            + ", ".join(str(key) for key in sorted(active))
        )
    completed.sort(key=lambda task: (
        -task["duration_ps"],
        task["start_ps"],
        task["tile_id"],
        task["task_id"],
    ))
    return completed


def analyze_global_timeline(completed):
    ordered = sorted(
        completed,
        key=lambda task: (
            task["start_ps"],
            task["finish_ps"],
            task["tile_id"],
            task["task_id"],
        ),
    )
    first = ordered[0]
    interval_start = first["start_ps"]
    interval_finish = first["finish_ps"]
    interval_end_task = first
    union_ps = 0
    gaps = []
    for task in ordered[1:]:
        if task["start_ps"] > interval_finish:
            union_ps += interval_finish - interval_start
            gaps.append(
                {
                    "start_ps": interval_finish,
                    "finish_ps": task["start_ps"],
                    "duration_ps": task["start_ps"] - interval_finish,
                    "after_tile_id": interval_end_task["tile_id"],
                    "after_task_id": interval_end_task["task_id"],
                    "before_tile_id": task["tile_id"],
                    "before_task_id": task["task_id"],
                }
            )
            interval_start = task["start_ps"]
            interval_finish = task["finish_ps"]
            interval_end_task = task
        elif task["finish_ps"] > interval_finish:
            interval_finish = task["finish_ps"]
            interval_end_task = task
    union_ps += interval_finish - interval_start
    gaps.sort(key=lambda gap: (
        -gap["duration_ps"],
        gap["start_ps"],
    ))
    return {
        "first_ps": ordered[0]["start_ps"],
        "last_ps": max(task["finish_ps"] for task in ordered),
        "union_ps": union_ps,
        "gaps": gaps,
    }


def write_events(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "sim_time_ps",
                "event",
                "tile_id",
                "task_id",
                "execution_id",
            ]
        )
        for record in records:
            writer.writerow(
                [
                    record["sim_time_ps"],
                    record["event"],
                    record["tile_id"],
                    record["task_id"],
                    record["execution_id"],
                ]
            )


def write_summary(path, completed):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "tile_id",
                "task_id",
                "execution_id",
                "start_ps",
                "finish_ps",
                "duration_ps",
            ]
        )
        for task in completed:
            writer.writerow(
                [
                    task["tile_id"],
                    task["task_id"],
                    task["execution_id"],
                    task["start_ps"],
                    task["finish_ps"],
                    task["duration_ps"],
                ]
            )


def write_gaps(path, gaps):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "start_ps",
                "finish_ps",
                "duration_ps",
                "after_tile_id",
                "after_task_id",
                "before_tile_id",
                "before_task_id",
            ]
        )
        for gap in gaps:
            writer.writerow(
                [
                    gap["start_ps"],
                    gap["finish_ps"],
                    gap["duration_ps"],
                    gap["after_tile_id"],
                    gap["after_task_id"],
                    gap["before_tile_id"],
                    gap["before_task_id"],
                ]
            )


def main():
    parser = argparse.ArgumentParser(
        description="Extract and validate Mittens task events from an SST log"
    )
    parser.add_argument(
        "trace_source",
        type=Path,
        help="SST log or directory containing per-tile trace CSV files",
    )
    parser.add_argument("events_csv", type=Path)
    parser.add_argument("summary_csv", type=Path)
    parser.add_argument("gaps_csv", type=Path)
    args = parser.parse_args()

    records = parse_trace(args.trace_source)
    if not records:
        raise RuntimeError("the simulation log contains no task trace events")
    completed = pair_tasks(records)
    timeline = analyze_global_timeline(completed)
    write_events(args.events_csv, records)
    write_summary(args.summary_csv, completed)
    write_gaps(args.gaps_csv, timeline["gaps"])

    span_ps = timeline["last_ps"] - timeline["first_ps"]
    gap_ps = span_ps - timeline["union_ps"]
    print(
        f"task trace: {len(completed)} tasks, "
        f"{len(records)} events, "
        f"span={span_ps / 1_000_000_000_000:.9f} s, "
        f"task_union={timeline['union_ps'] / 1_000_000_000_000:.9f} s, "
        f"global_gaps={gap_ps / 1_000_000_000_000:.9f} s"
    )
    for task in completed[:10]:
        print(
            "task trace longest: "
            f"tile={task['tile_id']} "
            f"task={task['task_id']} "
            f"execution={task['execution_id']} "
            f"duration={task['duration_ps'] / 1_000_000_000:.6f} ms"
        )
    for gap in timeline["gaps"][:10]:
        print(
            "task trace longest gap: "
            f"after=tile{gap['after_tile_id']}/task{gap['after_task_id']} "
            f"before=tile{gap['before_tile_id']}/task{gap['before_task_id']} "
            f"duration={gap['duration_ps'] / 1_000_000_000:.6f} ms"
        )


if __name__ == "__main__":
    main()
