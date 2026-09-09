#!/usr/bin/env python3
"""Validate the FAST Phase-3 mapping/placement differential end to end.

Phase 3 may change placement, so the final RAM/NoC route buckets are not
expected to be byte-identical.  It may not change semantic work or silently
lose traffic.  This validator therefore requires an identical pre-elision
inventory and proves, for each arm independently, that every descriptor,
logical transfer, physical request, and byte is partitioned exactly among the
remaining-RAM, retained-local, and direct-forward dispositions.  Completed SST
evidence must then reconcile with the selected disposition.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


UINT64_MAX = (1 << 64) - 1
SPM_BYTES = 2 * 1024 * 1024
FRAME_BYTES = 4096
GLOBAL_RAM_BYTES = 32 * 1024 * 1024 * 1024
EXPECTED_SST_THREADS = 16
EXPECTED_RAM_CHANNELS = 32
INVALID_U32 = (1 << 32) - 1

PRE_ELISION_METRICS = (
    "descriptor_count",
    "logical_bytes",
    "logical_transfer_count",
    "physical_request_count",
    "physical_byte_count",
)
SEMANTIC_COUNTERS = (
    "epoch_count",
    "materialized_consumer_region_count",
    "materialized_producer_region_count",
    "materialized_tensor_count",
    "materialized_tensor_bytes",
    "materialized_main_transfer_count_logical",
    "materialized_tail_transfer_count_logical",
    "maximum_live_global_ram_bytes",
    "preserved_external_spill_descriptor_count",
    "preserved_hybrid_spill_descriptor_count",
)
ZERO_CORRECTNESS_COUNTERS = (
    "cross_epoch_direct_route_count",
    "deferred_dependency_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "retained_local_capacity_fallback_count",
    "retained_local_epoch_closure_fallback_component_count",
    "retained_local_fallback_component_count",
    "retained_local_mixed_lattice_fallback_component_count",
    "unclassified_boundary_count",
    "unowned_materialized_byte_count",
    "zero_contribution_consumer_region_count",
)
STOP_PATTERN = re.compile(
    r"MITTENS_QEMU_CAPTURE_HOST stop_reason=([^\s(]+)\([0-9]+\) "
    r"count=([0-9]+)"
)
PROGRESS_FIELD_PATTERN = re.compile(r"([a-zA-Z0-9_]+)=([^ ]+)")
REGION_ASSIGNMENT_PATTERN = re.compile(
    r"#sculptor\.mapping_execution_region_assignment<([^>]*)>"
)
PHYSICAL_ASSIGNMENT_PATTERN = re.compile(
    r"#sculptor\.physical_tile_assignment<logicalTileId = ([0-9]+) : i64, "
    r"physicalTileId = ([0-9]+) : i64"
)


class DifferentialError(RuntimeError):
    """Missing, stale, malformed, or inconsistent Phase-3 evidence."""


def fail(message: str) -> None:
    raise DifferentialError(message)


def require_directory(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if path.is_symlink() or not resolved.is_dir():
        fail(f"{label} is missing or symbolic: {path}")
    return resolved


def require_file(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if path.is_symlink() or not resolved.is_file() or resolved.stat().st_size == 0:
        fail(f"{label} is missing, empty, or symbolic: {path}")
    return resolved


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with require_file(path, "evidence file").open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path, label: str) -> dict[str, Any]:
    path = require_file(path, label)
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read {label}: {error}")
    if not isinstance(payload, dict):
        fail(f"{label} must contain one JSON object")
    return payload


def read_one_csv_row(path: Path, label: str) -> dict[str, str]:
    path = require_file(path, label)
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        rows = list(reader)
    if reader.fieldnames is None or len(rows) != 1 or None in rows[0]:
        fail(f"{label} must contain one complete row")
    return rows[0]


def require_u64(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        fail(f"{label} must be an integer")
    if value < 0 or value > UINT64_MAX:
        fail(f"{label} is outside unsigned 64-bit range")
    return value


def require_counter(counters: dict[str, Any], name: str, label: str) -> int:
    if name not in counters:
        fail(f"{label} lacks counter {name}")
    return require_u64(counters[name], f"{label}.{name}")


def parse_i64_field(record: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)} = (-?[0-9]+) : i64\b", record)
    if match is None:
        fail(f"mapping region assignment lacks {name}")
    value = int(match.group(1))
    if value < 0:
        fail(f"mapping region assignment has negative {name}")
    return value


def disposition_record(counters: dict[str, Any], direction: str) -> dict[str, Any]:
    remaining_names = {
        "descriptor_count": f"materialized_{direction}_dma_descriptor_count",
        "logical_bytes": f"remaining_{direction}_logical_bytes",
        "logical_transfer_count": f"remaining_{direction}_logical_transfer_count",
        "physical_request_count": f"remaining_{direction}_physical_request_count",
        "physical_byte_count": f"remaining_{direction}_physical_byte_count",
    }
    record: dict[str, Any] = {}
    for metric in PRE_ELISION_METRICS:
        before = require_counter(
            counters,
            f"retained_{direction}_{metric}_before_elision",
            "materialization counters",
        )
        remaining = require_counter(
            counters, remaining_names[metric], "materialization counters"
        )
        retained = require_counter(
            counters,
            f"elided_retained_{direction}_{metric}",
            "materialization counters",
        )
        direct = require_counter(
            counters,
            f"direct_forward_elided_{direction}_{metric}",
            "materialization counters",
        )
        if remaining + retained + direct != before:
            fail(
                f"{direction} {metric} disposition does not cover pre-elision "
                f"inventory: {remaining}+{retained}+{direct}!={before}"
            )
        record[metric] = {
            "pre_elision": before,
            "remaining_global_ram": remaining,
            "retained_local": retained,
            "direct_forward": direct,
        }
    return record


def mapping_summary(run: Path, selected_regions: int) -> dict[str, Any]:
    mapping_path = require_file(
        run / "deployment" / "06-mapping-plan.mlir", "mapping plan"
    )
    placed_path = require_file(
        run / "deployment" / "08-placed.mlir", "placed mapping"
    )
    mapping = mapping_path.read_text(encoding="utf-8")
    placed = placed_path.read_text(encoding="utf-8")
    assignments: list[dict[str, int]] = []
    for raw in REGION_ASSIGNMENT_PATTERN.findall(mapping):
        entry = {
            name: parse_i64_field(raw, name)
            for name in (
                "regionId",
                "logicalTileId",
                "iterationBegin",
                "iterationEnd",
                "iterationStep",
                "ringSlots",
                "waveWidth",
                "estimatedPeakSPMBytes",
            )
        }
        if (
            entry["iterationBegin"] >= entry["iterationEnd"]
            or entry["iterationStep"] == 0
            or entry["ringSlots"] == 0
            or entry["waveWidth"] == 0
            or entry["estimatedPeakSPMBytes"] > SPM_BYTES
        ):
            fail("mapping region assignment has an invalid domain or capacity")
        assignments.append(entry)
    if len(assignments) != selected_regions:
        fail(
            f"mapping carries {len(assignments)} region assignments, expected "
            f"{selected_regions}"
        )
    region_ids = [entry["regionId"] for entry in assignments]
    if region_ids != sorted(set(region_ids)):
        fail("mapping region assignment IDs are duplicated or noncanonical")

    physical: dict[int, int] = {}
    for logical_raw, physical_raw in PHYSICAL_ASSIGNMENT_PATTERN.findall(placed):
        logical = int(logical_raw)
        physical_tile = int(physical_raw)
        if logical in physical and physical[logical] != physical_tile:
            fail("one logical tile has multiple physical placements")
        physical[logical] = physical_tile
    if any(entry["logicalTileId"] not in physical for entry in assignments):
        fail("a selected region logical tile lacks physical placement")
    if selected_regions:
        if 'decision = "selected"' not in mapping:
            fail("selected mapping lacks its execution-residency cost decision")
        if "profitable = false" in mapping:
            fail("selected mapping contains an unprofitable residency region")

    digital_work = sum(
        int(value) for value in re.findall(r"\bdigitalWork = ([0-9]+) : i64", mapping)
    )
    return {
        "mapping_sha256": sha256(mapping_path),
        "placed_sha256": sha256(placed_path),
        "leaf_assignment_count": mapping.count(
            "#sculptor.mapping_leaf_assignment<"
        ),
        "range_assignment_count": mapping.count(
            "#sculptor.mapping_iteration_range<"
        ),
        "analog_lane_assignment_occurrences": mapping.count("laneKind = analog"),
        "digital_lane_assignment_occurrences": mapping.count("laneKind = digital"),
        "digital_work": digital_work,
        "region_assignment_count": len(assignments),
        "region_ids": region_ids,
        "maximum_estimated_region_spm_bytes": max(
            (entry["estimatedPeakSPMBytes"] for entry in assignments), default=0
        ),
        "region_physical_tiles": {
            str(entry["regionId"]): physical[entry["logicalTileId"]]
            for entry in assignments
        },
    }


def load_compile(run_path: Path, expected_mode: str) -> dict[str, Any]:
    run = require_directory(run_path, f"{expected_mode} compile run")
    qualification = read_json(run / "compile-qualification.json", "qualification")
    if qualification.get("schema") != "sculptor.compile-qualification" or qualification.get("status") != "PASS":
        fail(f"{expected_mode} compile qualification is not PASS")
    checks = qualification.get("checks")
    if not isinstance(checks, dict) or any(
        not isinstance(value, dict) or value.get("status") != "PASS"
        for value in checks.values()
    ):
        fail(f"{expected_mode} compile qualification has a failed check")

    audit_path = run / "execution-residency-audit.json"
    audit = read_json(audit_path, "execution-residency audit")
    expected_phase = 0 if expected_mode == "off" else 3
    if (
        audit.get("schema") != "sculptor.execution-residency-audit"
        or audit.get("schema_version") != 1
        or audit.get("status") != "PASS"
        or audit.get("mode") != expected_mode
        or audit.get("phase") != expected_phase
        or audit.get("physical_change_count") != 0
        or audit.get("semantic_ir_changed") is not False
    ):
        fail(f"{expected_mode} execution-residency audit is stale or invalid")
    audit_check = checks.get("execution_residency_audit")
    if (
        not isinstance(audit_check, dict)
        or audit_check.get("mode") != expected_mode
        or audit_check.get("phase") != expected_phase
        or audit_check.get("audit_sha256") != sha256(audit_path)
    ):
        fail(f"{expected_mode} qualification is not bound to its residency audit")

    selected_regions = require_u64(
        audit.get("selected_region_count"), "selected_region_count"
    )
    selected_members = require_u64(
        audit.get("selected_member_count"), "selected_member_count"
    )
    selected_edges = require_u64(
        audit.get("selected_internal_edge_count"), "selected_internal_edge_count"
    )
    if expected_mode == "off":
        if any((selected_regions, selected_members, selected_edges)):
            fail("off control unexpectedly selected residency work")
    elif (
        selected_regions == 0
        or selected_members == 0
        or selected_edges == 0
        or selected_regions != audit.get("recommended_candidate_count")
    ):
        fail("select compile does not carry a nonempty committed region plan")

    deployment = read_json(run / "deployment-manifest.json", "deployment manifest")
    active_tiles = deployment.get("active_tile_ids")
    if (
        deployment.get("schema") != "sculptor.deployment"
        or not isinstance(active_tiles, list)
        or not active_tiles
        or active_tiles != sorted(set(active_tiles))
        or any(isinstance(tile, bool) or not isinstance(tile, int) for tile in active_tiles)
    ):
        fail(f"{expected_mode} deployment has an invalid active-tile set")

    architecture = read_json(run / "streaming-architecture.json", "architecture")
    if architecture.get("architecture") != {
        "fixed_shard_bytes": FRAME_BYTES,
        "global_ram_bytes": GLOBAL_RAM_BYTES,
        "max_in_flight": 2,
        "noc_word_bytes": 4,
        "scratchpad_bytes": SPM_BYTES,
        "version": 1,
    }:
        fail(f"{expected_mode} compile does not use the frozen architecture")

    materialization_path = run / "materialization-audit.json"
    materialization = read_json(materialization_path, "materialization audit")
    if (
        materialization.get("schema") != "sculptor.materialization-audit"
        or materialization.get("version") != 1
        or materialization.get("status") != "PASS"
        or materialization.get("errors") != []
        or materialization.get("maximum_frame_bytes") != FRAME_BYTES
        or materialization.get("active_tile_ids") != active_tiles
    ):
        fail(f"{expected_mode} materialization audit is incomplete")
    counters = materialization.get("counters")
    if not isinstance(counters, dict):
        fail(f"{expected_mode} materialization audit lacks counters")
    for name in ZERO_CORRECTNESS_COUNTERS:
        if require_counter(counters, name, expected_mode) != 0:
            fail(f"{expected_mode} materialization has nonzero {name}")
    if require_counter(counters, "phase4_accounting_tile_count", expected_mode) != len(active_tiles) or require_counter(counters, "phase5_accounting_tile_count", expected_mode) != len(active_tiles) or require_counter(counters, "retained_local_policy_enabled_tile_count", expected_mode) != len(active_tiles):
        fail(f"{expected_mode} materialization contracts do not cover every tile")

    memory = read_json(
        run / "memory-reports" / "tile-memory-summary.json", "tile-memory audit"
    )
    capacity = memory.get("capacity_gate")
    summaries = memory.get("summaries")
    maxima = (
        summaries.get("capacity", {}).get("maximums", {})
        if isinstance(summaries, dict)
        else {}
    )
    maximum_spm = maxima.get("requiredLocalBytes")
    if (
        memory.get("active_tile_count") != len(active_tiles)
        or memory.get("scratchpad_capacity_bytes") != SPM_BYTES
        or not isinstance(capacity, dict)
        or capacity.get("status") != "PASS"
        or capacity.get("errors") != []
        or isinstance(maximum_spm, bool)
        or not isinstance(maximum_spm, int)
        or not 0 <= maximum_spm <= SPM_BYTES
    ):
        fail(f"{expected_mode} tile-memory capacity proof is invalid")

    manifest = read_json(run / "run-manifest.json", "run manifest")
    parameters = manifest.get("parameters")
    if not isinstance(parameters, dict) or parameters.get(
        "execution_residency_mode"
    ) != expected_mode:
        fail(f"{expected_mode} run manifest has stale parameters")
    artifacts = manifest.get("artifacts")
    bound_materialization = (
        artifacts.get("materialization_audit") if isinstance(artifacts, dict) else None
    )
    if (
        not isinstance(bound_materialization, dict)
        or bound_materialization.get("sha256") != sha256(materialization_path)
    ):
        fail(f"{expected_mode} run manifest is not bound to materialization")

    disposition = {
        direction: disposition_record(counters, direction)
        for direction in ("input", "output")
    }
    mapping = mapping_summary(run, selected_regions)
    return {
        "directory": str(run),
        "active_tiles": active_tiles,
        "audit": audit,
        "audit_sha256": sha256(audit_path),
        "materialization_sha256": sha256(materialization_path),
        "run_manifest_sha256": sha256(run / "run-manifest.json"),
        "parameters": parameters,
        "counters": counters,
        "disposition": disposition,
        "maximum_spm_bytes": maximum_spm,
        "mapping": mapping,
    }


def parse_simulated_ns(value: str) -> float:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?) (ps|ns|us|ms|s)", value)
    if match is None:
        fail(f"invalid simulated time {value!r}")
    scale = {"ps": 1e-3, "ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
    return float(match.group(1)) * scale[match.group(2)]


def router_summary(path: Path) -> dict[str, int]:
    path = require_file(path, "router statistics")
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None:
            fail("router statistics have no header")
        totals: dict[str, int] = {}
        packet_hops = 0
        flit_hops = 0
        for row in reader:
            try:
                value = int(row["Sum.u64"])
            except (KeyError, TypeError, ValueError):
                fail("router statistics contain a malformed Sum.u64")
            component = row.get("ComponentName")
            statistic = row.get("StatisticName")
            if component == "global_ram":
                totals[str(statistic)] = value
            elif statistic == "packets_forwarded":
                packet_hops += value
            elif statistic == "flits_forwarded":
                flit_hops += value
    for required in ("requests", "bytes", "queue_delay_cycles", "service_cycles"):
        if required not in totals:
            fail(f"router statistics lack global_ram.{required}")
    return {**totals, "network_packet_hops": packet_hops, "network_flit_hops": flit_hops}


def load_sst(evidence_path: Path, compile_data: dict[str, Any]) -> dict[str, Any]:
    evidence = require_directory(evidence_path, "SST evidence")
    result = read_one_csv_row(evidence / "result.csv", "SST result")
    status = read_one_csv_row(evidence / "status.csv", "SST status")
    if result.get("status") != "PASS" or status.get("status") != "PASS" or status.get("exit_code") != "0":
        fail(f"SST did not terminate successfully: {evidence}")
    if (
        result.get("active_tiles") != str(len(compile_data["active_tiles"]))
        or result.get("epoch_count")
        != str(require_counter(compile_data["counters"], "epoch_count", "compile"))
        or result.get("synchronization_mode") != "exact_dependencies"
        or result.get("global_ram_channels") != str(EXPECTED_RAM_CHANNELS)
    ):
        fail(f"SST result has stale architecture or synchronization fields: {evidence}")
    simulated_ns = parse_simulated_ns(result.get("simulated_time", ""))
    try:
        wall_seconds = float(result["simulation_wall_seconds"])
    except (KeyError, ValueError):
        fail(f"SST result has an invalid wall time: {evidence}")
    if wall_seconds <= 0:
        fail(f"SST result has a nonpositive wall time: {evidence}")

    launch = read_json(evidence / "launch.json", "SST launch")
    if (
        launch.get("source_compile_directory") != compile_data["directory"]
        or launch.get("materialization_audit_sha256")
        != compile_data["materialization_sha256"]
        or launch.get("source_run_manifest_sha256")
        != compile_data["run_manifest_sha256"]
        or launch.get("sst_threads") != EXPECTED_SST_THREADS
        or launch.get("sst_partitioner") != "sst.simple"
        or launch.get("global_ram_channels") != EXPECTED_RAM_CHANNELS
        or launch.get("partition_global_dma") is not True
    ):
        fail(f"SST launch is not bound to the frozen Phase-3 compile/host policy: {evidence}")
    partition = read_json(evidence / "partition-summary.json", "partition summary")
    if (
        partition.get("status") != "PASS"
        or partition.get("active_tile_count") != len(compile_data["active_tiles"])
        or partition.get("requested_threads") != EXPECTED_SST_THREADS
        or partition.get("occupied_active_tile_threads") != EXPECTED_SST_THREADS
    ):
        fail(f"SST partition does not occupy all requested threads: {evidence}")

    log_path = require_file(evidence / "simulation.log", "SST terminal log")
    log = log_path.read_text(encoding="utf-8", errors="replace")
    active_tiles = compile_data["active_tiles"]
    init_tiles = sorted(int(value) for value in re.findall(r"SCULPTOR_RA_INIT_PASS tile=([0-9]+)", log))
    if init_tiles != active_tiles or log.count("SCULPTOR_RA_SIM_PASS") != len(active_tiles) or "SCULPTOR_RA_SIM_FAIL" in log or "Simulation failed" in log:
        fail(f"SST terminal tile accounting is incomplete: {evidence}")

    terminal: dict[int, dict[str, str]] = {}
    for line in log.splitlines():
        if not line.startswith("SCULPTOR_RA_PROGRESS kind=2 "):
            continue
        fields = dict(PROGRESS_FIELD_PATTERN.findall(line))
        if "tile" not in fields:
            fail("terminal progress row lacks tile ID")
        terminal[int(fields["tile"])] = fields
    if sorted(terminal) != active_tiles:
        fail(f"SST lacks a terminal progress row for every tile: {evidence}")
    epoch_count = require_counter(compile_data["counters"], "epoch_count", "compile")
    for tile, fields in terminal.items():
        required = {
            "issued",
            "retired",
            "physical_global_ram_dma_submitted",
            "physical_global_ram_dma_completed",
            "current_epoch",
            "earliest_incomplete_epoch",
            "active_count",
            "active_loop",
            "next_issue",
            "ready_queue",
            "pending_receive",
            "pending_transmit",
            "pending_dma",
            "active_receive_states",
            "reported_receive_states",
        }
        if not required.issubset(fields):
            fail(f"tile {tile} terminal row lacks required counters")
        if (
            int(fields["current_epoch"]) != epoch_count
            or int(fields["earliest_incomplete_epoch"]) != INVALID_U32
            or int(fields["active_loop"]) != INVALID_U32
            or int(fields["next_issue"]) != UINT64_MAX
            or any(
                int(fields[name]) != 0
                for name in (
                    "active_count",
                    "ready_queue",
                    "pending_transmit",
                    "pending_dma",
                    "active_receive_states",
                    "reported_receive_states",
                )
            )
        ):
            fail(f"tile {tile} did not terminate quiescently")

    terminal_totals = {
        name: sum(int(fields[name]) for fields in terminal.values())
        for name in (
            "issued",
            "retired",
            "physical_global_ram_dma_submitted",
            "physical_global_ram_dma_completed",
            "pending_receive",
        )
    }
    expected_requests = require_counter(
        compile_data["counters"], "materialized_input_physical_request_count", "compile"
    ) + require_counter(
        compile_data["counters"], "materialized_output_physical_request_count", "compile"
    )
    if (
        terminal_totals["physical_global_ram_dma_submitted"] != expected_requests
        or terminal_totals["physical_global_ram_dma_completed"] != expected_requests
    ):
        fail(f"SST physical DMA does not reconcile with compile audit: {evidence}")

    stop_counts: dict[str, int] = {}
    for reason, raw_count in STOP_PATTERN.findall(log):
        if reason in stop_counts:
            fail(f"duplicate stop-reason summary {reason}: {evidence}")
        stop_counts[reason] = int(raw_count)
    if stop_counts.get("guest-exit") != len(active_tiles) or stop_counts.get("analog-submit", 0) <= 0:
        fail(f"SST stop-reason accounting is incomplete: {evidence}")

    router = router_summary(evidence / "router-statistics.csv")
    expected_bytes = require_counter(
        compile_data["counters"], "materialized_input_physical_byte_count", "compile"
    ) + require_counter(
        compile_data["counters"], "materialized_output_physical_byte_count", "compile"
    )
    if router["requests"] != expected_requests or router["bytes"] != expected_bytes:
        fail(f"SST Global RAM counters do not reconcile with compile audit: {evidence}")
    return {
        "directory": str(evidence),
        "result_sha256": sha256(evidence / "result.csv"),
        "simulation_log_sha256": sha256(log_path),
        "simulated_time": result["simulated_time"],
        "simulated_ns": simulated_ns,
        "wall_seconds": wall_seconds,
        "terminal_totals": terminal_totals,
        "stop_counts": stop_counts,
        "router": router,
    }


def compare(control: dict[str, Any], selected: dict[str, Any]) -> dict[str, Any]:
    if control["active_tiles"] != selected["active_tiles"]:
        fail("selected active-tile set differs from control")
    if control["audit"].get("source_ir_sha256") != selected["audit"].get("source_ir_sha256"):
        fail("selected and control residency audits have different source IR")

    control_parameters = dict(control["parameters"])
    selected_parameters = dict(selected["parameters"])
    control_parameters.pop("execution_residency_mode", None)
    selected_parameters.pop("execution_residency_mode", None)
    if control_parameters != selected_parameters:
        fail("compile parameters differ beyond execution-residency mode")

    semantic = {}
    for name in SEMANTIC_COUNTERS:
        left = require_counter(control["counters"], name, "control")
        right = require_counter(selected["counters"], name, "selected")
        if left != right:
            fail(f"semantic materialization counter differs: {name}")
        semantic[name] = left

    pre_elision = {}
    for direction in ("input", "output"):
        pre_elision[direction] = {}
        for metric in PRE_ELISION_METRICS:
            name = f"retained_{direction}_{metric}_before_elision"
            left = require_counter(control["counters"], name, "control")
            right = require_counter(selected["counters"], name, "selected")
            if left != right:
                fail(f"pre-elision inventory differs: {name}")
            pre_elision[direction][metric] = left

    leaf_fields = (
        "leaf_assignment_count",
        "range_assignment_count",
        "analog_lane_assignment_occurrences",
        "digital_lane_assignment_occurrences",
        "digital_work",
    )
    leaf_accounting = {}
    for name in leaf_fields:
        left = control["mapping"][name]
        right = selected["mapping"][name]
        if left != right:
            fail(f"mapping leaf accounting differs: {name}")
        leaf_accounting[name] = left

    route_delta = {}
    for direction in ("input", "output"):
        route_delta[direction] = {}
        for metric in PRE_ELISION_METRICS:
            route_delta[direction][metric] = {
                bucket: selected["disposition"][direction][metric][bucket]
                - control["disposition"][direction][metric][bucket]
                for bucket in (
                    "remaining_global_ram",
                    "retained_local",
                    "direct_forward",
                )
            }
    return {
        "active_tile_count": len(control["active_tiles"]),
        "source_ir_sha256": control["audit"].get("source_ir_sha256"),
        "semantic_counters": semantic,
        "pre_elision_inventory": pre_elision,
        "leaf_accounting": leaf_accounting,
        "route_disposition_delta_selected_minus_control": route_delta,
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    control = load_compile(args.control_compile, "off")
    selected = load_compile(args.selected_compile, "select")
    compile_comparison = compare(control, selected)
    control_sst = load_sst(args.control_sst, control)
    selected_sst = load_sst(args.selected_sst, selected)
    for field in ("issued", "retired"):
        if control_sst["terminal_totals"][field] != selected_sst["terminal_totals"][field]:
            fail(f"completed SST semantic work differs: {field}")
    if control_sst["stop_counts"].get("analog-submit") != selected_sst["stop_counts"].get("analog-submit"):
        fail("completed SST analog submission counts differ")

    simulated_delta = selected_sst["simulated_ns"] - control_sst["simulated_ns"]
    wall_delta = selected_sst["wall_seconds"] - control_sst["wall_seconds"]
    return {
        "schema": "golem.sculptor-fast-phase3-differential",
        "schema_version": 1,
        "status": "PASS",
        "interpretation": (
            "semantic and pre-elision work are identical; placement-dependent "
            "RAM, retained-local, and direct-forward buckets may redistribute "
            "only when each arm's disposition identity and SST accounting close exactly"
        ),
        "compile": {
            "comparison": compile_comparison,
            "control": {
                "directory": control["directory"],
                "audit_sha256": control["audit_sha256"],
                "materialization_sha256": control["materialization_sha256"],
                "maximum_spm_bytes": control["maximum_spm_bytes"],
                "mapping": control["mapping"],
                "disposition": control["disposition"],
            },
            "selected": {
                "directory": selected["directory"],
                "audit_sha256": selected["audit_sha256"],
                "materialization_sha256": selected["materialization_sha256"],
                "selected_region_count": selected["audit"]["selected_region_count"],
                "selected_member_count": selected["audit"]["selected_member_count"],
                "selected_internal_edge_count": selected["audit"]["selected_internal_edge_count"],
                "maximum_spm_bytes": selected["maximum_spm_bytes"],
                "mapping": selected["mapping"],
                "disposition": selected["disposition"],
            },
        },
        "sst": {
            "control": control_sst,
            "selected": selected_sst,
            "semantic_work": {
                "issued": control_sst["terminal_totals"]["issued"],
                "retired": control_sst["terminal_totals"]["retired"],
                "analog_commands_submitted": control_sst["stop_counts"]["analog-submit"],
            },
            "selected_minus_control": {
                "simulated_ns": simulated_delta,
                "simulated_percent": 100.0 * simulated_delta / control_sst["simulated_ns"],
                "wall_seconds": wall_delta,
                "wall_percent": 100.0 * wall_delta / control_sst["wall_seconds"],
            },
        },
        "errors": [],
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    if path.is_symlink():
        fail(f"report path is symbolic: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-compile", type=Path, required=True)
    parser.add_argument("--selected-compile", type=Path, required=True)
    parser.add_argument("--control-sst", type=Path, required=True)
    parser.add_argument("--selected-sst", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = build_report(args)
        write_report(args.output, report)
    except (DifferentialError, OSError) as error:
        print(f"FAST Phase-3 differential: FAIL: {error}", file=sys.stderr)
        return 1
    delta = report["sst"]["selected_minus_control"]
    print(
        "FAST Phase-3 differential: PASS "
        f"regions={report['compile']['selected']['selected_region_count']} "
        f"simulated_delta={delta['simulated_percent']:+.2f}% "
        f"wall_delta={delta['wall_percent']:+.2f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
