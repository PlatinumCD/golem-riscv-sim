#!/usr/bin/env python3
"""Fit Amdahl and Universal Scalability Law models to SST evidence."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def parse_key_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value
    return values


def read_point(directory: Path) -> dict[str, object]:
    launch = json.loads((directory / "launch.json").read_text(encoding="utf-8"))
    resources = parse_key_values(directory / "resource-usage.txt")
    with (directory / "result.csv").open(encoding="utf-8", newline="") as stream:
        result = next(csv.DictReader(stream))
    threads = launch.get("sst_threads")
    if not isinstance(threads, int) or isinstance(threads, bool) or threads <= 0:
        raise ValueError(f"{directory}: launch has no positive sst_threads")
    wall = float(resources["sst_wall_seconds"])
    user = float(resources["sst_user_seconds"])
    system = float(resources["sst_system_seconds"])
    partition_path = directory / "partition-summary.json"
    occupied = None
    if partition_path.is_file():
        partition = json.loads(partition_path.read_text(encoding="utf-8"))
        occupied = partition.get("occupied_active_tile_threads")
    return {
        "directory": str(directory.resolve()),
        "threads": threads,
        "wall_seconds": wall,
        "user_seconds": user,
        "system_seconds": system,
        "cpu_seconds": user + system,
        "voluntary_context_switches": int(resources["context_switches_voluntary"]),
        "involuntary_context_switches": int(resources["context_switches_involuntary"]),
        "simulated_time": result["simulated_time"],
        "occupied_active_tile_threads": occupied,
    }


def linear_fit(points: list[tuple[float, float]]) -> tuple[float, float]:
    count = len(points)
    if count < 2:
        raise ValueError("at least two points are required for a linear fit")
    sum_x = sum(x for x, _ in points)
    sum_y = sum(y for _, y in points)
    sum_xx = sum(x * x for x, _ in points)
    sum_xy = sum(x * y for x, y in points)
    denominator = count * sum_xx - sum_x * sum_x
    if denominator == 0:
        raise ValueError("scaling points do not span distinct thread counts")
    slope = (count * sum_xy - sum_x * sum_y) / denominator
    intercept = (sum_y - slope * sum_x) / count
    return intercept, slope


def fit_models(points: list[dict[str, object]], target_seconds: float) -> dict[str, object]:
    serial = [point for point in points if point["threads"] == 1]
    if len(serial) != 1:
        raise ValueError("evidence must contain exactly one one-thread baseline")
    baseline = serial[0]
    baseline_wall = float(baseline["wall_seconds"])
    simulated_times = {str(point["simulated_time"]) for point in points}
    if len(simulated_times) != 1:
        raise ValueError("evidence points do not have identical simulated time")

    records = []
    for point in sorted(points, key=lambda item: int(item["threads"])):
        threads = int(point["threads"])
        wall = float(point["wall_seconds"])
        speedup = baseline_wall / wall
        karp_flatt = None
        if threads > 1:
            karp_flatt = (1.0 / speedup - 1.0 / threads) / (1.0 - 1.0 / threads)
        record = dict(point)
        record.update({
            "speedup": speedup,
            "parallel_efficiency": speedup / threads,
            "karp_flatt_serial_fraction": karp_flatt,
        })
        records.append(record)

    amdahl_intercept, amdahl_parallel = linear_fit([
        (1.0 / int(point["threads"]), float(point["wall_seconds"]))
        for point in points
    ])
    amdahl_serial_fraction = amdahl_intercept / (
        amdahl_intercept + amdahl_parallel
    )

    usl_points = []
    for point in records:
        threads = int(point["threads"])
        if threads == 1:
            continue
        speedup = float(point["speedup"])
        y_value = (threads / speedup - 1.0) / (threads - 1.0)
        usl_points.append((float(threads), y_value))
    if len(usl_points) == 1:
        usl_alpha = usl_points[0][1]
        usl_beta = 0.0
    else:
        usl_alpha, usl_beta = linear_fit(usl_points)

    projections = []
    for threads in (1, 2, 4, 8, 16, 20, 32):
        amdahl_wall = amdahl_intercept + amdahl_parallel / threads
        usl_speedup = threads / (
            1.0
            + usl_alpha * (threads - 1.0)
            + usl_beta * threads * (threads - 1.0)
        )
        projections.append({
            "threads": threads,
            "amdahl_wall_seconds": amdahl_wall,
            "usl_wall_seconds": baseline_wall / usl_speedup,
            "usl_speedup": usl_speedup,
        })

    best = min(records, key=lambda record: float(record["wall_seconds"]))
    return {
        "schema": "golem.sculptor-sst-scaling",
        "schema_version": 1,
        "status": "PASS",
        "simulated_time": simulated_times.pop(),
        "target_wall_seconds": target_seconds,
        "baseline_wall_seconds": baseline_wall,
        "best_observed": {
            "threads": best["threads"],
            "wall_seconds": best["wall_seconds"],
            "speedup": best["speedup"],
            "additional_speedup_to_target": float(best["wall_seconds"]) / target_seconds,
        },
        "amdahl": {
            "serial_seconds": amdahl_intercept,
            "parallel_seconds": amdahl_parallel,
            "serial_fraction": amdahl_serial_fraction,
            "asymptotic_wall_seconds": amdahl_intercept,
        },
        "universal_scalability_law": {
            "contention_alpha": usl_alpha,
            "coherency_beta": usl_beta,
        },
        "points": records,
        "projections": projections,
    }


def write_markdown(result: dict[str, object], path: Path) -> None:
    lines = [
        "# SST host-parallel scaling model",
        "",
        f"Modeled completion is fixed at `{result['simulated_time']}`. "
        f"The host-wall target is `{result['target_wall_seconds']:.2f} s`.",
        "",
        "| SST threads | Wall (s) | Speedup | Efficiency | CPU-s | Involuntary switches |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for point in result["points"]:
        lines.append(
            f"| {point['threads']} | {point['wall_seconds']:.2f} | "
            f"{point['speedup']:.3f}x | "
            f"{100.0 * point['parallel_efficiency']:.1f}% | "
            f"{point['cpu_seconds']:.2f} | "
            f"{point['involuntary_context_switches']} |"
        )
    amdahl = result["amdahl"]
    usl = result["universal_scalability_law"]
    best = result["best_observed"]
    lines.extend([
        "",
        f"Amdahl fit: `{amdahl['serial_seconds']:.2f} s` serial and "
        f"`{amdahl['parallel_seconds']:.2f} s` parallel "
        f"(`{100.0 * amdahl['serial_fraction']:.1f}%` fitted serial fraction).",
        f"USL fit: alpha=`{usl['contention_alpha']:.5f}`, "
        f"beta=`{usl['coherency_beta']:.5f}`.",
        f"Best observed is `{best['wall_seconds']:.2f} s` at "
        f"`{best['threads']}` threads; reaching the target still requires "
        f"`{best['additional_speedup_to_target']:.2f}x`.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", nargs="+", type=Path)
    parser.add_argument("--target-seconds", type=float, default=10.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.target_seconds) or args.target_seconds <= 0:
        raise SystemExit("target seconds must be finite and positive")
    result = fit_models(
        [read_point(directory) for directory in args.evidence],
        args.target_seconds,
    )
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if args.markdown is not None:
        write_markdown(result, args.markdown)
    print(
        "SST scaling model: PASS "
        f"(best={result['best_observed']['wall_seconds']:.2f}s, "
        f"target={args.target_seconds:.2f}s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
