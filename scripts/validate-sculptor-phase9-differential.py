#!/usr/bin/env python3
"""Validate the Phase 9 exact-dependency/bulk-barrier differential gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


class DifferentialError(RuntimeError):
    """A malformed artifact or a failed Phase 9 differential invariant."""


UPSTREAM_STAGES = (
    "01-canonical.mlir",
    "02-converted.mlir",
    "03-layouts.mlir",
    "03-golem.mlir",
    "03-duplicate-matrices.mlir",
    "03-resolved-layouts.mlir",
    "04-expanded-digital-work.mlir",
    "04-parametric-work.mlir",
    "04-tensor-fragments.mlir",
    "04-residency-regions.mlir",
    "05-ra-tree.mlir",
    "06-mapping-plan.mlir",
    "08-placed.mlir",
)
MOVEMENT_FIELDS = (
    "physical_global_ram_dma_requests",
    "physical_global_ram_dma_completions",
    "physical_global_ram_dma_bytes",
    "physical_noc_frames_sent",
    "physical_noc_frames_received",
    "physical_noc_payload_bytes_sent",
    "physical_noc_payload_bytes_received",
    "local_copy_transfers",
    "local_copy_bytes",
    "retained_forwarded_logical_transfers",
    "retained_forwarded_logical_bytes",
)
TERMINAL_FIELDS = (
    "issued",
    "executed",
    "retired",
    "active_max",
    "active_loop",
    "next_issue",
    "active_retired",
    "active_total",
    "active_count",
    "ring_slots",
    "ready_queue",
    "pending_receive",
    "pending_transmit",
    "pending_dma",
    "wait_loop",
    "wait_route",
    "wait_iteration",
    "wait_slot",
    "wait_phase",
    "active_receive_states",
    "reported_receive_states",
)
PROFILE_WORK_FIELDS = (
    "analog_active_cycles",
    "analog_link_beats",
    "network_packets",
    "network_words",
    "network_word_hops",
)
KEY_VALUE_PATTERN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([0-9]+)")
BARRIER_PATTERN = re.compile(
    r"MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=([0-9]+) "
    r"released_epoch=([0-9]+) arrivals=([0-9]+)"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_file(path: Path) -> Path:
    if not path.is_file():
        raise DifferentialError(f"missing required file: {path}")
    return path


def load_json(path: Path) -> Any:
    try:
        return json.loads(require_file(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DifferentialError(f"cannot read JSON {path}: {error}") from error


def one_csv_row(path: Path) -> dict[str, str]:
    try:
        with require_file(path).open(encoding="utf-8", newline="") as source:
            rows = list(csv.DictReader(source))
    except (OSError, csv.Error) as error:
        raise DifferentialError(f"cannot read CSV {path}: {error}") from error
    if len(rows) != 1:
        raise DifferentialError(f"expected one data row in {path}, found {len(rows)}")
    return rows[0]


def integer_fields(line: str) -> dict[str, int]:
    return {name: int(value) for name, value in KEY_VALUE_PATTERN.findall(line)}


def load_active_tiles(compile_dir: Path) -> list[int]:
    path = require_file(compile_dir / "active-cores.txt")
    try:
        tiles = [int(line) for line in path.read_text(encoding="utf-8").splitlines()]
    except ValueError as error:
        raise DifferentialError(f"invalid active-core list {path}: {error}") from error
    if not tiles or tiles != sorted(set(tiles)):
        raise DifferentialError(f"active-core list is empty, unsorted, or duplicated: {path}")
    return tiles


def load_uart(
    evidence_dir: Path, active_tiles: list[int]
) -> tuple[dict[int, dict[str, int]], dict[int, dict[str, int]]]:
    movement: dict[int, dict[str, int]] = {}
    terminal: dict[int, dict[str, int]] = {}
    for tile in active_tiles:
        path = require_file(evidence_dir / "uart" / f"tile-{tile}.log")
        lines = path.read_text(encoding="utf-8").splitlines()
        if sum(line.startswith("SCULPTOR_RA_INIT_PASS ") for line in lines) != 1:
            raise DifferentialError(f"tile {tile} does not have one initialization pass")
        if lines.count("SCULPTOR_RA_SIM_PASS") != 1:
            raise DifferentialError(f"tile {tile} does not have one simulation pass")
        movement_lines = [
            line for line in lines if line.startswith("SCULPTOR_RA_MOVEMENT ")
        ]
        terminal_lines = [
            line for line in lines if line.startswith("SCULPTOR_RA_PROGRESS kind=2 ")
        ]
        if len(movement_lines) != 1 or len(terminal_lines) != 1:
            raise DifferentialError(
                f"tile {tile} must have one movement and one terminal record"
            )
        movement_record = integer_fields(movement_lines[0])
        terminal_record = integer_fields(terminal_lines[0])
        missing_movement = set(MOVEMENT_FIELDS) - movement_record.keys()
        missing_terminal = set(TERMINAL_FIELDS) - terminal_record.keys()
        if missing_movement or missing_terminal:
            raise DifferentialError(
                f"tile {tile} has incomplete terminal accounting: "
                f"movement={sorted(missing_movement)}, terminal={sorted(missing_terminal)}"
            )
        movement[tile] = {field: movement_record[field] for field in MOVEMENT_FIELDS}
        terminal[tile] = {field: terminal_record[field] for field in TERMINAL_FIELDS}
    return movement, terminal


def aggregate(records: dict[int, dict[str, int]]) -> dict[str, int]:
    fields = next(iter(records.values())).keys()
    return {field: sum(record[field] for record in records.values()) for field in fields}


def load_statistics(path: Path) -> dict[str, int]:
    selected: dict[str, int] = {}
    router_totals = {"packets_forwarded": 0, "flits_forwarded": 0}
    try:
        with require_file(path).open(encoding="utf-8", newline="") as source:
            for row in csv.DictReader(source):
                component = row["ComponentName"]
                name = row["StatisticName"]
                value = int(row["Sum.u64"])
                if component == "global_ram":
                    selected[f"global_ram_{name}"] = value
                elif component.startswith("router_") and name in router_totals:
                    router_totals[name] += value
    except (OSError, csv.Error, KeyError, ValueError) as error:
        raise DifferentialError(f"cannot read statistics {path}: {error}") from error
    selected.update({f"router_{name}": value for name, value in router_totals.items()})
    return selected


def load_profile(profile_dir: Path, active_tiles: list[int]) -> dict[str, int]:
    totals: dict[str, int] = {
        "instructions": 0,
        "vector_instructions": 0,
        "cpu_cycles": 0,
        "synchronization_events": 0,
        "synchronization_grants": 0,
        "stop_epoch_barrier_arrive": 0,
        **{field: 0 for field in PROFILE_WORK_FIELDS},
    }
    performance_dir = profile_dir / "trace" / "performance"
    for tile in active_tiles:
        path = require_file(performance_dir / f"tile-{tile}-summary.csv")
        try:
            with path.open(encoding="utf-8", newline="") as source:
                values = {row["metric"]: int(row["value"]) for row in csv.DictReader(source)}
        except (OSError, csv.Error, KeyError, ValueError) as error:
            raise DifferentialError(f"cannot read performance summary {path}: {error}") from error
        if values.get("tile_id") != tile:
            raise DifferentialError(f"performance summary tile mismatch: {path}")
        for field in totals:
            if field not in values:
                raise DifferentialError(f"performance summary lacks {field}: {path}")
            totals[field] += values[field]
    return totals


def normalize_environment(manifest: dict[str, Any]) -> dict[str, Any]:
    environment = dict(manifest.get("environment", {}))
    environment.pop("GOLEM_MODEL_OUTPUT_DIR", None)
    environment.pop("GOLEM_MODEL_RUN_ID", None)
    return environment


def percentage_reduction(new: float, old: float) -> float:
    if old <= 0:
        raise DifferentialError("comparison baseline must be positive")
    return 100.0 * (old - new) / old


def validate(arguments: argparse.Namespace) -> dict[str, Any]:
    exact_compile = arguments.exact_compile.resolve()
    exact_sst = arguments.exact_sst.resolve()
    bulk_compile = arguments.bulk_compile.resolve()
    bulk_sst = arguments.bulk_sst.resolve()

    exact_status = one_csv_row(exact_sst / "status.csv")
    bulk_status = one_csv_row(bulk_sst / "status.csv")
    exact_result = one_csv_row(exact_sst / "result.csv")
    bulk_result = one_csv_row(bulk_sst / "result.csv")
    for label, status, result in (
        ("exact", exact_status, exact_result),
        ("bulk", bulk_status, bulk_result),
    ):
        if status.get("status") != "PASS" or result.get("status") != "PASS":
            raise DifferentialError(f"{label} execution did not pass")
    if exact_result.get("synchronization_mode") != "exact_dependencies":
        raise DifferentialError("exact execution did not use exact_dependencies")
    if bulk_result.get("synchronization_mode") != "bulk_barrier":
        raise DifferentialError("bulk execution did not use bulk_barrier")

    exact_manifest = load_json(exact_compile / "run-manifest.json")
    bulk_manifest = load_json(bulk_compile / "run-manifest.json")
    if exact_manifest.get("hardware") != bulk_manifest.get("hardware"):
        raise DifferentialError("exact and bulk hardware configurations differ")
    if normalize_environment(exact_manifest) != normalize_environment(bulk_manifest):
        raise DifferentialError("exact and bulk workload environments differ")
    exact_model = exact_manifest.get("run", {}).get("model")
    if not exact_model or exact_model != bulk_manifest.get("run", {}).get("model"):
        raise DifferentialError("exact and bulk models differ")
    exact_tools = {
        name: record.get("sha256") for name, record in exact_manifest.get("tools", {}).items()
    }
    bulk_tools = {
        name: record.get("sha256") for name, record in bulk_manifest.get("tools", {}).items()
    }
    if exact_tools != bulk_tools:
        raise DifferentialError("exact and bulk tool hashes differ")

    stage_hashes: dict[str, str] = {}
    for stage in UPSTREAM_STAGES:
        exact_path = require_file(exact_compile / "deployment" / stage)
        bulk_path = require_file(bulk_compile / "deployment" / stage)
        exact_hash = sha256(exact_path)
        if exact_hash != sha256(bulk_path):
            raise DifferentialError(f"exact and bulk compiler work differs at {stage}")
        stage_hashes[stage] = exact_hash

    exact_tiles = load_active_tiles(exact_compile)
    bulk_tiles = load_active_tiles(bulk_compile)
    if exact_tiles != bulk_tiles:
        raise DifferentialError("exact and bulk active-tile sets differ")
    tile_count = len(exact_tiles)
    epoch_count = int(exact_result["epoch_count"])
    if int(bulk_result["epoch_count"]) != epoch_count:
        raise DifferentialError("exact and bulk semantic epoch counts differ")

    exact_deployment = load_json(exact_compile / "deployment-manifest.json")
    bulk_deployment = load_json(bulk_compile / "deployment-manifest.json")
    if exact_deployment.get("synchronization") != {
        "mode": "exact_dependencies",
        "semantic_epoch_count": epoch_count,
    }:
        raise DifferentialError("invalid exact deployment synchronization contract")
    if bulk_deployment.get("synchronization") != {
        "mode": "bulk_barrier",
        "semantic_epoch_count": epoch_count,
    }:
        raise DifferentialError("invalid bulk deployment synchronization contract")

    exact_audit = load_json(exact_compile / "materialization-audit.json")
    bulk_audit = load_json(bulk_compile / "materialization-audit.json")
    exact_counters = exact_audit.get("counters")
    if not isinstance(exact_counters, dict) or exact_counters != bulk_audit.get("counters"):
        raise DifferentialError("exact and bulk materialization accounting differs")

    exact_movement, exact_terminal = load_uart(exact_sst, exact_tiles)
    bulk_movement, bulk_terminal = load_uart(bulk_sst, bulk_tiles)
    if exact_movement != bulk_movement:
        raise DifferentialError("exact and bulk per-tile physical movement differs")
    if exact_terminal != bulk_terminal:
        raise DifferentialError("exact and bulk per-tile terminal accounting differs")
    movement = aggregate(exact_movement)
    terminal = aggregate(exact_terminal)

    exact_log = require_file(exact_sst / "simulation.log").read_text(encoding="utf-8")
    bulk_log = require_file(bulk_sst / "simulation.log").read_text(encoding="utf-8")
    if BARRIER_PATTERN.search(exact_log):
        raise DifferentialError("exact execution used the global epoch barrier")
    releases = [tuple(map(int, match)) for match in BARRIER_PATTERN.findall(bulk_log)]
    expected_releases = [(epoch, epoch + 1, tile_count) for epoch in range(epoch_count)]
    if releases != expected_releases:
        raise DifferentialError("bulk execution has an incomplete or malformed release sequence")

    exact_stats = load_statistics(exact_sst / "router-statistics.csv")
    bulk_stats = load_statistics(bulk_sst / "router-statistics.csv")
    for field in (
        "global_ram_requests",
        "global_ram_bytes",
        "router_packets_forwarded",
        "router_flits_forwarded",
    ):
        if exact_stats.get(field) != bulk_stats.get(field):
            raise DifferentialError(f"exact and bulk {field} differ")
    if exact_stats.get("global_ram_requests") != movement["physical_global_ram_dma_requests"]:
        raise DifferentialError("global-RAM and runtime DMA request accounting differ")
    if exact_stats.get("global_ram_bytes") != movement["physical_global_ram_dma_bytes"]:
        raise DifferentialError("global-RAM and runtime DMA byte accounting differ")
    if movement["physical_global_ram_dma_requests"] != movement[
        "physical_global_ram_dma_completions"
    ]:
        raise DifferentialError("not every submitted global DMA completed")
    if movement["physical_noc_frames_sent"] != movement["physical_noc_frames_received"]:
        raise DifferentialError("NoC frame send/receive accounting differs")
    if movement["physical_noc_payload_bytes_sent"] != movement[
        "physical_noc_payload_bytes_received"
    ]:
        raise DifferentialError("NoC payload send/receive accounting differs")

    readiness = {
        name.removeprefix("global_ram_readiness_"): value
        for name, value in exact_stats.items()
        if name.startswith("global_ram_readiness_")
    }
    required_readiness = {
        "blocked_reads",
        "releases",
        "interval_lookups",
        "publications",
        "duplicate_publications",
        "maximum_waiters",
        "execution_teardowns",
    }
    if required_readiness - readiness.keys():
        raise DifferentialError("exact execution lacks readiness statistics")
    if readiness["blocked_reads"] <= 0 or readiness["blocked_reads"] != readiness["releases"]:
        raise DifferentialError("blocked-read and readiness-release events do not reconcile")
    if readiness["publications"] != exact_counters.get(
        "materialized_output_physical_request_count"
    ):
        raise DifferentialError("readiness publications do not reconcile with physical writes")
    if readiness["duplicate_publications"] != 0:
        raise DifferentialError("exact execution has duplicate readiness publications")
    if readiness["maximum_waiters"] > readiness["blocked_reads"]:
        raise DifferentialError("maximum readiness waiters exceed blocked reads")
    if readiness["execution_teardowns"] != 1:
        raise DifferentialError("exact execution did not tear down readiness state exactly once")

    exact_profile = load_profile(arguments.exact_profile.resolve(), exact_tiles)
    bulk_profile = load_profile(arguments.bulk_profile.resolve(), bulk_tiles)
    for field in PROFILE_WORK_FIELDS:
        if exact_profile[field] != bulk_profile[field]:
            raise DifferentialError(f"exact and bulk profiled work differs for {field}")
    if exact_profile["stop_epoch_barrier_arrive"] != 0:
        raise DifferentialError("exact profile contains barrier arrivals")
    expected_arrivals = tile_count * epoch_count
    if bulk_profile["stop_epoch_barrier_arrive"] != expected_arrivals:
        raise DifferentialError("bulk profile barrier arrivals do not match tiles times epochs")

    exact_simulated_ms = float(exact_result["simulated_time"].split()[0])
    bulk_simulated_ms = float(bulk_result["simulated_time"].split()[0])
    exact_sst_wall = float(exact_result["simulation_wall_seconds"])
    bulk_sst_wall = float(bulk_result["simulation_wall_seconds"])
    exact_total_wall = float(exact_status["total_wall_seconds"])
    bulk_total_wall = float(bulk_status["total_wall_seconds"])

    report = {
        "schema": "golem.sculptor-phase9-differential",
        "schema_version": 1,
        "status": "PASS",
        "model": exact_model,
        "active_tile_count": tile_count,
        "semantic_epoch_count": epoch_count,
        "same_tree_proof": {
            "tool_sha256": exact_tools,
            "compiler_stage_sha256": stage_hashes,
        },
        "exact": {
            "simulated_time_ms": exact_simulated_ms,
            "sst_wall_seconds": exact_sst_wall,
            "total_wall_seconds": exact_total_wall,
            "guest_instructions": exact_profile["instructions"],
            "guest_vector_instructions": exact_profile["vector_instructions"],
            "guest_cpu_cycles": exact_profile["cpu_cycles"],
            "global_epoch_barrier_arrivals": 0,
            "readiness": readiness,
        },
        "bulk": {
            "simulated_time_ms": bulk_simulated_ms,
            "sst_wall_seconds": bulk_sst_wall,
            "total_wall_seconds": bulk_total_wall,
            "guest_instructions": bulk_profile["instructions"],
            "guest_vector_instructions": bulk_profile["vector_instructions"],
            "guest_cpu_cycles": bulk_profile["cpu_cycles"],
            "global_epoch_barrier_arrivals": expected_arrivals,
            "global_epoch_barrier_releases": epoch_count,
        },
        "improvement": {
            "same_tree_guest_instruction_reduction_percent": percentage_reduction(
                exact_profile["instructions"], bulk_profile["instructions"]
            ),
            "same_tree_simulated_time_reduction_percent": percentage_reduction(
                exact_simulated_ms, bulk_simulated_ms
            ),
            "same_tree_sst_wall_reduction_percent": percentage_reduction(
                exact_sst_wall, bulk_sst_wall
            ),
            "frozen_phase8_simulated_time_reduction_percent": percentage_reduction(
                exact_simulated_ms, arguments.phase8_simulated_ms
            ),
            "frozen_phase8_sst_wall_reduction_percent": percentage_reduction(
                exact_sst_wall, arguments.phase8_sst_wall_seconds
            ),
        },
        "identical_accounting": {
            "runtime_terminal": {
                field: terminal[field]
                for field in (
                    "issued",
                    "executed",
                    "retired",
                    "active_count",
                    "ready_queue",
                    "pending_receive",
                    "pending_transmit",
                    "pending_dma",
                    "active_receive_states",
                )
            },
            "physical_movement": movement,
            "network": {
                "router_packets_forwarded": exact_stats["router_packets_forwarded"],
                "router_flits_forwarded": exact_stats["router_flits_forwarded"],
            },
            "profiled_work": {
                field: exact_profile[field] for field in PROFILE_WORK_FIELDS
            },
            "materialization": exact_counters,
        },
    }
    return report


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exact_compile", type=Path)
    parser.add_argument("exact_sst", type=Path)
    parser.add_argument("bulk_compile", type=Path)
    parser.add_argument("bulk_sst", type=Path)
    parser.add_argument("--exact-profile", required=True, type=Path)
    parser.add_argument("--bulk-profile", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--phase8-simulated-ms", type=float, default=6.92043)
    parser.add_argument("--phase8-sst-wall-seconds", type=float, default=78.577430)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        report = validate(arguments)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (DifferentialError, OSError, ValueError) as error:
        print(f"phase 9 differential validation failed: {error}", file=sys.stderr)
        return 1
    print(
        "Phase 9 exact/bulk differential: PASS "
        f"({report['exact']['simulated_time_ms']:.6g} ms exact, "
        f"{report['bulk']['simulated_time_ms']:.6g} ms bulk)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
