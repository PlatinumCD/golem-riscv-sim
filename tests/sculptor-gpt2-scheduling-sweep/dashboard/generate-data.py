#!/usr/bin/env python3
"""Generate dependency-free JavaScript data for the scheduling dashboard."""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import statistics
from pathlib import Path


DISPLAY_NAMES = {
    "random-seed0": "Random · seed 0",
    "snake": "Snake",
    "greedy-l2": "Greedy · L2",
    "greedy-l3": "Greedy · L3",
    "greedy-b8": "Greedy · B8",
    "greedy-timing-l2": "Greedy timing · L2",
    "greedy-timing-l3": "Greedy timing · L3",
    "greedy-timing-b8": "Greedy timing · B8",
    "greedy-b8-link-pressure": "Greedy · B8 · link pressure",
    "greedy-b8-balanced-reductions": "Greedy · B8 · balanced reductions",
    "greedy-b8-link-pressure-balanced-reductions":
        "Greedy · B8 · link + reductions",
    "greedy-timing-b8-link-pressure":
        "Greedy timing · B8 · link pressure",
    "greedy-timing-b8-balanced-reductions":
        "Greedy timing · B8 · balanced reductions",
    "greedy-timing-b8-link-pressure-balanced-reductions":
        "Greedy timing · B8 · link + reductions",
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--configurations", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def optional_integer(value: str) -> int | None:
    return int(value) if value else None


def heuristic_integer(heuristic: str, name: str) -> int | None:
    match = re.search(rf"(?:^|,){re.escape(name)}=(\d+)(?:,|$)", heuristic)
    return int(match.group(1)) if match else None


def load_configurations(path: Path) -> list[dict[str, object]]:
    configurations: list[dict[str, object]] = []
    with path.open(encoding="utf-8", newline="") as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].lstrip().startswith("#"):
                continue
            if len(row) != 4:
                raise ValueError(
                    f"{path}: expected four tab-separated fields, got {len(row)}"
                )
            name, schedule, heuristic, balanced = row
            configurations.append(
                {
                    "id": name,
                    "label": DISPLAY_NAMES.get(name, name),
                    "schedule": schedule,
                    "heuristic": heuristic,
                    "lookahead": heuristic_integer(heuristic, "lookahead"),
                    "beamWidth": heuristic_integer(heuristic, "beam"),
                    "linkPressure": "link-pressure" in heuristic.split(","),
                    "balancedReductions": balanced == "1",
                }
            )
    return configurations


def parse_tile_profiles(path: Path) -> dict[str, object]:
    if not path.is_file():
        return {}

    profiles: list[dict[str, int]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "MITTENS_PROFILE" not in line:
            continue
        profiles.append(
            {
                name: int(value)
                for name, value in re.findall(r"(\w+)=([0-9]+)", line)
            }
        )
    if not profiles:
        return {}

    # An instantiated but unused tile retires only the 35-instruction startup
    # stub. Exclude those stubs from load-balance statistics.
    active = [
        profile
        for profile in profiles
        if profile.get("instructions", 0) > 35
    ]
    if not active:
        active = profiles
    cycles = [profile.get("cpu_cycles", 0) for profile in active]
    mean_cycles = statistics.fmean(cycles)

    def total(name: str) -> int:
        return sum(profile.get(name, 0) for profile in profiles)

    return {
        "profileTileCount": len(profiles),
        "activeProfileTiles": len(active),
        "maxTileCycles": max(cycles),
        "meanTileCycles": mean_cycles,
        "tileCycleCv": (
            statistics.pstdev(cycles) / mean_cycles if mean_cycles else 0.0
        ),
        "loadEfficiency": (
            sum(cycles) / (len(cycles) * max(cycles))
            if cycles and max(cycles)
            else 0.0
        ),
        "maxTileInstructions": max(
            profile.get("instructions", 0) for profile in active
        ),
        "maxTileNetworkWords": max(
            profile.get("network_tx_words", 0) for profile in active
        ),
        "analogSet": total("analog_set"),
        "analogLoad": total("analog_load"),
        "analogCompute": total("analog_compute"),
        "analogStore": total("analog_store"),
        "analogMove": total("analog_move"),
        "analogInputWords": total("analog_input_words"),
        "analogOutputWords": total("analog_output_words"),
        "rxDmaActiveCycles": total("rx_dma_active_cycles"),
    }


def resolve_simulation_log(
    results_path: Path,
    row: dict[str, str],
) -> Path:
    recorded = Path(row["simulation_log"])
    if recorded.is_file():
        return recorded
    return (
        results_path.parent
        / f"tokens-{row['tokens']}"
        / "configurations"
        / row["configuration"]
        / row["compute_mode"]
        / "deployment"
        / "simulation.log"
    )


def load_results(path: Path, known: set[str]) -> list[dict[str, object]]:
    # QEMU UART uses CRLF. Older result files can contain the trailing CR from
    # GPT2_OUTPUT in checksum_bits, so strip it before parsing the CSV.
    with path.open(encoding="utf-8", newline="") as stream:
        text = stream.read().replace("\r", "")

    results: list[dict[str, object]] = []
    seen: set[tuple[int, str, str]] = set()
    for row in csv.DictReader(io.StringIO(text, newline="")):
        configuration = row["configuration"]
        if configuration not in known:
            raise ValueError(f"{path}: unknown configuration {configuration!r}")
        key = (
            int(row["tokens"]),
            row["compute_mode"],
            configuration,
        )
        if key in seen:
            raise ValueError(f"{path}: duplicate result {key}")
        seen.add(key)
        runtime_ns = optional_integer(row["simulated_time_ns"])
        simulation_log = resolve_simulation_log(path, row)
        profile = parse_tile_profiles(simulation_log)
        result: dict[str, object] = {
            "tokens": key[0],
            "mode": key[1],
            "configuration": key[2],
            "status": row["status"],
            "runtimeNs": runtime_ns,
            "activeCores": optional_integer(row["active_cores"]),
            "taskCount": optional_integer(row["task_count"]),
            "dependencyCount": optional_integer(row["dependency_count"]),
            "logicalArrays": optional_integer(row["logical_arrays"]),
            "transferBytes": optional_integer(
                row["inter_core_transfer_bytes"]
            ),
            "graphScore": optional_integer(row["graph_score"]),
            "outputElements": optional_integer(row["output_elements"]),
            "finiteElements": optional_integer(row["finite_elements"]),
            "firstBits": optional_integer(row["first_bits"]),
            "checksumBits": optional_integer(row["checksum_bits"]),
            "instructions": optional_integer(row["total_instructions"]),
            "vectorInstructions": optional_integer(
                row["total_vector_instructions"]
            ),
            "cpuCycles": optional_integer(row["total_cpu_cycles"]),
            "networkWords": optional_integer(
                row["total_network_tx_words"]
            ),
            "analogCycles": optional_integer(
                row["total_analog_active_cycles"]
            ),
            "simulationLog": str(simulation_log),
            **profile,
        }
        if (
            runtime_ns is not None
            and isinstance(profile.get("maxTileCycles"), int)
            and profile["maxTileCycles"]
        ):
            result["dependencyStretch"] = (
                runtime_ns / profile["maxTileCycles"]
            )
        else:
            result["dependencyStretch"] = None
        results.append(result)
    return results


def main() -> None:
    arguments = parse_arguments()
    configurations = load_configurations(arguments.configurations)
    results = load_results(
        arguments.results,
        {configuration["id"] for configuration in configurations},
    )
    missing_profiles = sum(
        result["status"] == "pass" and "maxTileCycles" not in result
        for result in results
    )
    if missing_profiles:
        raise ValueError(
            f"{arguments.results}: missing tile profiles for "
            f"{missing_profiles} passing runs"
        )
    payload = {
        "title": "GPT-2 Scheduling Sweep",
        "tokens": sorted({result["tokens"] for result in results}),
        "modes": ["analog", "digital"],
        "configurations": configurations,
        "results": results,
        "summary": {
            "total": len(results),
            "passed": sum(result["status"] == "pass" for result in results),
            "failed": sum(result["status"] == "failed" for result in results),
            "profiled": sum("maxTileCycles" in result for result in results),
        },
    }

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(payload, indent=2, separators=(",", ": "))
    arguments.output.write_text(
        f"window.GPT2_SWEEP_DATA = {encoded};\n",
        encoding="utf-8",
    )
    print(
        f"generated {arguments.output} "
        f"({payload['summary']['passed']}/{payload['summary']['total']} passed)"
    )


if __name__ == "__main__":
    main()
