#!/usr/bin/env python3
"""Produce a strict whole-model residency feasibility certificate.

The certificate joins immutable compiler artifacts with one completed SST run.
It validates execution completeness and architectural accounting, but it does
not inspect or compare model output values.  The resulting lower bounds answer
whether a target is ruled out by the current amount of work; they are not a
claim that a target is achievable without changing that work.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


PASS = "PASS"
COMPILE_PASS = "COMPILE_PASS"
CERTIFICATE_SCHEMA = "golem.residency-feasibility"
CERTIFICATE_VERSION = 1
SUPPORTED_MAXIMUM_FRAME_BYTES = {
    4096,
    8192,
    16384,
    32768,
    65536,
    131072,
    262144,
}

MATERIALIZATION_CORRECTNESS_COUNTERS = (
    "unowned_materialized_byte_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "unclassified_boundary_count",
)

MATERIALIZATION_ACCOUNTING_COUNTERS = (
    "epoch_count",
    "materialized_tensor_count",
    "materialized_tensor_bytes",
    "materialized_producer_region_count",
    "materialized_consumer_region_count",
    "materialized_output_dma_descriptor_count",
    "materialized_input_dma_descriptor_count",
    "materialized_main_transfer_count_logical",
    "materialized_tail_transfer_count_logical",
    "retained_local_owner_alias_count",
    "elided_retained_input_descriptor_count",
    "elided_retained_input_logical_bytes",
    "elided_retained_input_logical_transfer_count",
    "elided_retained_output_descriptor_count",
    "elided_retained_output_logical_bytes",
    "elided_retained_output_logical_transfer_count",
    "cross_epoch_direct_route_count",
    "maximum_live_global_ram_bytes",
    *MATERIALIZATION_CORRECTNESS_COUNTERS,
)

PERFORMANCE_METRICS = (
    "instructions",
    "cpu_cycles",
    "vector_instructions",
    "synchronization_events",
    "analog_active_cycles",
    "analog_link_beats",
    "network_packets",
    "network_words",
    "network_word_hops",
    "network_transit_ticks",
    "network_endpoint_queue_ticks",
    "receive_dma_active_cycles",
    "wait_epoch-barrier-arrive_ticks",
    "wait_scratchpad-dma-submit_ticks",
    "wait_scratchpad-dma-wait_ticks",
)

EPOCH_PATTERN = re.compile(
    r"^MITTENS_EPOCH_BARRIER_RELEASE "
    r"completed_epoch=(?P<completed>[0-9]+) "
    r"released_epoch=(?P<released>[0-9]+) "
    r"arrivals=(?P<arrivals>[0-9]+) "
    r"idle=(?P<idle>[0-9]+) "
    r"first_arrival_cycle=(?P<first>[0-9]+) "
    r"release_cycle=(?P<release>[0-9]+)$"
)

MOVEMENT_PREFIX = "SCULPTOR_RA_MOVEMENT "
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
PHASE4_MOVEMENT_AUDIT_FIELDS = (
    "phase4_accounting_tile_count",
    "retained_local_policy_enabled_tile_count",
    "retained_local_logical_transfer_count",
    "retained_local_logical_bytes",
    "materialized_input_physical_request_count",
    "materialized_output_physical_request_count",
    "materialized_input_physical_byte_count",
    "materialized_output_physical_byte_count",
    "retained_input_physical_request_count_before_elision",
    "retained_output_physical_request_count_before_elision",
    "retained_input_physical_byte_count_before_elision",
    "retained_output_physical_byte_count_before_elision",
    "elided_retained_input_physical_request_count",
    "elided_retained_output_physical_request_count",
    "elided_retained_input_physical_byte_count",
    "elided_retained_output_physical_byte_count",
)


class AuditError(RuntimeError):
    """Raised when evidence cannot support a feasibility certificate."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise AuditError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditError(f"cannot read JSON {path}: {error}") from error
    _require(isinstance(payload, dict), f"JSON root is not an object: {path}")
    return payload


def _csv_rows(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as source:
            return list(csv.DictReader(source))
    except OSError as error:
        raise AuditError(f"cannot read CSV {path}: {error}") from error


def _single_csv_row(path: Path) -> dict[str, str]:
    rows = _csv_rows(path)
    _require(len(rows) == 1, f"expected exactly one row in {path}, found {len(rows)}")
    return rows[0]


def _integer(value: Any, context: str) -> int:
    _require(not isinstance(value, bool), f"{context} must be an integer")
    try:
        converted = int(value)
    except (TypeError, ValueError) as error:
        raise AuditError(f"{context} must be an integer, got {value!r}") from error
    _require(str(converted) == str(value).strip(), f"{context} is not canonical: {value!r}")
    return converted


def _nonnegative_integer(value: Any, context: str) -> int:
    converted = _integer(value, context)
    _require(converted >= 0, f"{context} must be nonnegative")
    return converted


def _positive_integer(value: Any, context: str) -> int:
    converted = _integer(value, context)
    _require(converted > 0, f"{context} must be positive")
    return converted


def _frequency_hz(value: str) -> float:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)(Hz|kHz|MHz|GHz)", value)
    _require(match is not None, f"unsupported clock frequency: {value!r}")
    scales = {"Hz": 1.0, "kHz": 1e3, "MHz": 1e6, "GHz": 1e9}
    assert match is not None
    return float(match.group(1)) * scales[match.group(2)]


def _time_ms(value: str) -> float:
    match = re.fullmatch(
        r"\s*([0-9]+(?:\.[0-9]+)?)\s*(ps|ns|us|ms|s)\s*", value
    )
    _require(match is not None, f"unsupported simulated time: {value!r}")
    scales = {"ps": 1e-9, "ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}
    assert match is not None
    return float(match.group(1)) * scales[match.group(2)]


def _active_tiles(path: Path) -> list[int]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise AuditError(f"cannot read active-core manifest {path}: {error}") from error
    tiles = [_nonnegative_integer(line.strip(), f"{path} tile id") for line in lines if line.strip()]
    _require(bool(tiles), f"active-core manifest is empty: {path}")
    _require(len(tiles) == len(set(tiles)), f"active-core manifest has duplicate tile ids: {path}")
    _require(tiles == sorted(tiles), f"active-core manifest must be sorted: {path}")
    return tiles


def _status(path: Path, expected: str) -> dict[str, Any]:
    row = _single_csv_row(path)
    _require(row.get("status") == expected, f"{path} status is not {expected}")
    _require(_integer(row.get("exit_code"), f"{path} exit_code") == 0, f"{path} exit_code is nonzero")
    _require(not row.get("failure_stage"), f"{path} records failure stage {row.get('failure_stage')!r}")
    return {
        "status": expected,
        "exit_code": 0,
        "wall_seconds": float(row["total_wall_seconds"]),
    }


def _materialization(
    compile_dir: Path, active_tiles: list[int], epoch_count: int
) -> tuple[dict[str, Any], dict[str, int]]:
    path = compile_dir / "materialization-audit.json"
    payload = _json(path)
    _require(payload.get("schema") == "sculptor.materialization-audit", "invalid materialization-audit schema")
    _require(payload.get("audit_schema_version") == 1, "unsupported materialization-audit version")
    _require(payload.get("status") == PASS, "materialization audit did not pass")
    _require(payload.get("errors") == [], "materialization audit contains errors")
    _require(payload.get("active_tile_ids") == active_tiles, "materialization active tiles do not match")
    counters = payload.get("counters")
    _require(isinstance(counters, dict), "materialization counters are missing")
    values = {
        name: _nonnegative_integer(counters.get(name), f"materialization counter {name}")
        for name in MATERIALIZATION_ACCOUNTING_COUNTERS
    }
    _require(values["epoch_count"] == epoch_count, "materialization epoch count does not match deployment")
    for name in MATERIALIZATION_CORRECTNESS_COUNTERS:
        _require(values[name] == 0, f"materialization correctness counter {name} is nonzero")
    return payload, values


def _phase4_materialization(
    payload: dict[str, Any], active_tiles: list[int], maximum_frame_bytes: int
) -> dict[str, Any] | None:
    counters = payload.get("counters")
    _require(isinstance(counters, dict), "materialization counters are missing")

    # Any Phase-4-only key commits the producer to the complete schema.  Older
    # immutable Phase-2/3 evidence has none of these keys; a lone elision field
    # is corruption, not a reason to silently fall back to legacy parsing.
    present = {
        name for name in PHASE4_MOVEMENT_AUDIT_FIELDS if name in counters
    }
    if not present:
        return None
    missing = sorted(set(PHASE4_MOVEMENT_AUDIT_FIELDS) - set(counters))
    _require(
        not missing,
        "materialization audit has an incomplete Phase-4 accounting schema: "
        + ", ".join(missing),
    )
    values = {
        name: _nonnegative_integer(
            counters.get(name), f"Phase-4 materialization counter {name}"
        )
        for name in PHASE4_MOVEMENT_AUDIT_FIELDS
    }
    if values["phase4_accounting_tile_count"] == 0:
        nonzero = {
            name: value
            for name, value in values.items()
            if name != "phase4_accounting_tile_count" and value != 0
        }
        _require(
            not nonzero,
            "zero Phase-4 accounting tile count has nonzero counters: "
            + ", ".join(f"{name}={value}" for name, value in sorted(nonzero.items())),
        )
        return None
    _require(
        values["phase4_accounting_tile_count"] == len(active_tiles),
        "Phase-4 accounting tile count does not match active tiles",
    )
    _require(
        values["retained_local_policy_enabled_tile_count"]
        in (0, len(active_tiles)),
        "Phase-4 retained-local policy is enabled on only a subset of active tiles",
    )

    def validate_conservation(record: dict[str, int], context: str) -> None:
        for direction in ("input", "output"):
            for metric, remaining_suffix, elided_suffix in (
                ("requests", "request_count", "request_count"),
                ("bytes", "byte_count", "byte_count"),
            ):
                before_name = (
                    f"retained_{direction}_physical_{remaining_suffix}_before_elision"
                )
                remaining_name = (
                    f"materialized_{direction}_physical_{remaining_suffix}"
                )
                elided_name = (
                    f"elided_retained_{direction}_physical_{elided_suffix}"
                )
                before = record[before_name]
                remaining = record[remaining_name]
                elided = record[elided_name]
                _require(
                    before == remaining + elided,
                    f"{context} {direction} physical {metric} are not conserved: "
                    f"before={before}, remaining={remaining}, elided={elided}",
                )
            for stage, request_name, byte_name in (
                (
                    "before elision",
                    f"retained_{direction}_physical_request_count_before_elision",
                    f"retained_{direction}_physical_byte_count_before_elision",
                ),
                (
                    "remaining",
                    f"materialized_{direction}_physical_request_count",
                    f"materialized_{direction}_physical_byte_count",
                ),
                (
                    "elided",
                    f"elided_retained_{direction}_physical_request_count",
                    f"elided_retained_{direction}_physical_byte_count",
                ),
            ):
                requests = record[request_name]
                physical_bytes = record[byte_name]
                # Elision is a delta, not an absolute request stream.  Removing
                # one member of a compact pair can save payload bytes while the
                # before/after request count remains one.
                if stage == "elided":
                    _require(
                        requests == 0 or physical_bytes != 0,
                        f"{context} {direction} {stage} physical requests "
                        "have no removed payload bytes",
                    )
                    continue
                _require(
                    (requests == 0) == (physical_bytes == 0),
                    f"{context} {direction} {stage} physical requests and "
                    "bytes disagree on empty work",
                )
                _require(
                    physical_bytes
                    <= requests * maximum_frame_bytes,
                    f"{context} {direction} {stage} physical bytes exceed "
                    "the configured physical-frame envelope",
                )

    validate_conservation(values, "global Phase-4 accounting")
    _require(
        values["retained_local_logical_bytes"]
        == values["elided_retained_input_physical_byte_count"],
        "Phase-4 forwarded logical bytes disagree with elided input physical bytes",
    )

    tile_fields = tuple(
        name
        for name in PHASE4_MOVEMENT_AUDIT_FIELDS
        if name != "phase4_accounting_tile_count"
    )
    tile_records = payload.get("tiles")
    _require(
        isinstance(tile_records, list),
        "Phase-4 materialization audit has no per-tile accounting",
    )
    records_by_tile: dict[int, dict[str, int]] = {}
    for ordinal, raw_record in enumerate(tile_records):
        _require(
            isinstance(raw_record, dict),
            f"Phase-4 tile accounting record {ordinal} is not an object",
        )
        tile_id = _nonnegative_integer(
            raw_record.get("tile_id"), f"Phase-4 tile accounting record {ordinal} ID"
        )
        _require(
            tile_id not in records_by_tile,
            f"duplicate Phase-4 tile accounting record for tile {tile_id}",
        )
        missing_tile_fields = sorted(set(tile_fields) - set(raw_record))
        _require(
            not missing_tile_fields,
            f"Phase-4 tile {tile_id} accounting is incomplete: "
            + ", ".join(missing_tile_fields),
        )
        record = {
            name: _nonnegative_integer(
                raw_record.get(name), f"Phase-4 tile {tile_id} counter {name}"
            )
            for name in tile_fields
        }
        validate_conservation(record, f"Phase-4 tile {tile_id} accounting")
        records_by_tile[tile_id] = record
    _require(
        sorted(records_by_tile) == active_tiles,
        "Phase-4 per-tile accounting does not exactly match active tiles",
    )
    for name in tile_fields:
        _require(
            sum(record[name] for record in records_by_tile.values()) == values[name],
            f"Phase-4 per-tile counter {name} does not sum to its global value",
        )

    if values["retained_local_policy_enabled_tile_count"] == 0:
        retained_activity_fields = (
            "retained_local_logical_transfer_count",
            "retained_local_logical_bytes",
            "elided_retained_input_physical_request_count",
            "elided_retained_output_physical_request_count",
            "elided_retained_input_physical_byte_count",
            "elided_retained_output_physical_byte_count",
        )
        _require(
            all(values[name] == 0 for name in retained_activity_fields),
            "disabled retained-local policy reports Phase-4 retention activity",
        )
        return None

    directions = {
        direction: {
            "before_requests": values[
                f"retained_{direction}_physical_request_count_before_elision"
            ],
            "before_bytes": values[
                f"retained_{direction}_physical_byte_count_before_elision"
            ],
            "remaining_requests": values[
                f"materialized_{direction}_physical_request_count"
            ],
            "remaining_bytes": values[
                f"materialized_{direction}_physical_byte_count"
            ],
            "elided_requests": values[
                f"elided_retained_{direction}_physical_request_count"
            ],
            "elided_bytes": values[
                f"elided_retained_{direction}_physical_byte_count"
            ],
        }
        for direction in ("input", "output")
    }
    remaining_requests = sum(
        record["remaining_requests"] for record in directions.values()
    )
    remaining_bytes = sum(
        record["remaining_bytes"] for record in directions.values()
    )
    elided_requests = sum(
        record["elided_requests"] for record in directions.values()
    )
    elided_bytes = sum(record["elided_bytes"] for record in directions.values())
    before_requests = sum(
        record["before_requests"] for record in directions.values()
    )
    before_bytes = sum(record["before_bytes"] for record in directions.values())
    return {
        "tile_count": len(records_by_tile),
        "counters": values,
        "directions": directions,
        "physical_global_ram": {
            "before_requests": before_requests,
            "before_bytes": before_bytes,
            "remaining_requests": remaining_requests,
            "remaining_bytes": remaining_bytes,
            "elided_requests": elided_requests,
            "elided_bytes": elided_bytes,
        },
    }


def _memory_summary(compile_dir: Path, active_tiles: list[int]) -> dict[str, Any]:
    payload = _json(compile_dir / "memory-reports" / "tile-memory-summary.json")
    _require(payload.get("schema_version") == 1, "unsupported tile-memory summary version")
    _require(payload.get("active_tile_count") == len(active_tiles), "tile-memory active count does not match")
    tiles = payload.get("tiles")
    _require(isinstance(tiles, dict), "per-tile memory summaries are missing")
    try:
        memory_tile_ids = sorted(int(tile_id) for tile_id in tiles)
    except ValueError as error:
        raise AuditError("per-tile memory summary has a non-integer tile id") from error
    _require(memory_tile_ids == active_tiles, "per-tile memory summaries do not exactly match active tiles")
    summaries = payload.get("summaries")
    _require(isinstance(summaries, dict), "tile-memory summaries are missing")
    for section in ("audit", "capacity", "finalized", "physical"):
        _require(isinstance(summaries.get(section), dict), f"tile-memory {section} summary is missing")
        _require(isinstance(summaries[section].get("maximums"), dict), f"tile-memory {section} maximums are missing")
    audit = summaries["audit"]["maximums"]
    for name in (
        "escaping_allocation_count",
        "missing_deallocation_count",
        "unplanned_allocation_count",
        "unplanned_copy_count",
        "unplanned_full_tensor_copy_count",
    ):
        _require(_nonnegative_integer(audit.get(name), f"memory audit {name}") == 0, f"memory audit {name} is nonzero")
    return payload


def _performance(evidence_dir: Path, active_tiles: list[int]) -> dict[str, Any]:
    paths = sorted((evidence_dir / "trace" / "performance").glob("tile-*-summary.csv"))
    path_by_tile: dict[int, Path] = {}
    totals = Counter()
    maximums = Counter()
    for path in paths:
        match = re.fullmatch(r"tile-([0-9]+)-summary\.csv", path.name)
        _require(match is not None, f"unexpected performance summary name: {path}")
        assert match is not None
        tile_id = int(match.group(1))
        _require(tile_id not in path_by_tile, f"duplicate performance summary for tile {tile_id}")
        path_by_tile[tile_id] = path
        rows = _csv_rows(path)
        values: dict[str, int] = {}
        for row in rows:
            metric = row.get("metric", "")
            _require(metric not in values, f"duplicate metric {metric!r} in {path}")
            values[metric] = _nonnegative_integer(row.get("value"), f"{path} metric {metric}")
        _require(values.get("tile_id") == tile_id, f"tile_id metric does not match {path.name}")
        for metric in PERFORMANCE_METRICS:
            _require(metric in values, f"required metric {metric} is missing from {path}")
            totals[metric] += values[metric]
            maximums[metric] = max(maximums[metric], values[metric])
    _require(sorted(path_by_tile) == active_tiles, "performance summaries do not exactly match active tiles")
    return {
        "tile_count": len(paths),
        "totals": {metric: totals[metric] for metric in PERFORMANCE_METRICS},
        "maximums": {metric: maximums[metric] for metric in PERFORMANCE_METRICS},
    }


def _epochs(
    evidence_dir: Path, active_tile_count: int, epoch_count: int
) -> tuple[list[dict[str, int]], str]:
    path = evidence_dir / "simulation.log"
    try:
        log = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise AuditError(f"cannot read simulation log {path}: {error}") from error
    records: list[dict[str, int]] = []
    previous_release = 0
    for line in log.splitlines():
        match = EPOCH_PATTERN.fullmatch(line)
        if match is None:
            continue
        record = {name: int(value) for name, value in match.groupdict().items()}
        record["active"] = record["arrivals"] - record["idle"]
        record["interval_cycles"] = record["release"] - previous_release
        previous_release = record["release"]
        records.append(record)
    _require(len(records) == epoch_count, f"expected {epoch_count} epoch releases, found {len(records)}")
    _require([row["completed"] for row in records] == list(range(epoch_count)), "completed epochs are not sequential")
    _require([row["released"] for row in records] == list(range(1, epoch_count + 1)), "released epochs are not sequential")
    for row in records:
        _require(row["arrivals"] == active_tile_count, f"epoch {row['completed']} arrival count does not match active tiles")
        _require(0 <= row["idle"] <= row["arrivals"], f"epoch {row['completed']} idle count is invalid")
        _require(row["active"] > 0, f"epoch {row['completed']} has no working tile")
        _require(row["first"] <= row["release"], f"epoch {row['completed']} releases before its first arrival")
        _require(row["interval_cycles"] > 0, f"epoch {row['completed']} release cycle did not advance")
    _require(log.count("SCULPTOR_RA_SIM_PASS") == active_tile_count, "simulation pass count does not match active tiles")
    _require("Simulation is complete" in log, "simulation completion marker is missing")
    return records, log


def _movement_record(line: str, tile_id: int) -> dict[str, int]:
    tokens = line.split()
    _require(
        tokens and tokens[0] == MOVEMENT_PREFIX.strip(),
        f"tile {tile_id} has a malformed movement summary",
    )
    fields: dict[str, int] = {}
    for token in tokens[1:]:
        _require(
            token.count("=") == 1,
            f"tile {tile_id} movement summary has a malformed field {token!r}",
        )
        name, raw_value = token.split("=", 1)
        _require(
            name and name not in fields,
            f"tile {tile_id} movement summary has duplicate field {name!r}",
        )
        fields[name] = _nonnegative_integer(
            raw_value, f"tile {tile_id} movement field {name}"
        )
    expected = {"tile", *MOVEMENT_FIELDS}
    _require(
        set(fields) == expected,
        f"tile {tile_id} movement fields disagree: "
        f"missing={sorted(expected - set(fields))}, "
        f"unexpected={sorted(set(fields) - expected)}",
    )
    _require(
        fields["tile"] == tile_id,
        f"tile {tile_id} movement summary names tile {fields['tile']}",
    )
    return {name: fields[name] for name in MOVEMENT_FIELDS}


def _uart_completion(
    evidence_dir: Path, active_tiles: list[int], require_movement: bool
) -> dict[str, Any] | None:
    actual: list[int] = []
    movement_by_tile: dict[int, dict[str, int]] = {}
    movement_seen = False
    for path in sorted((evidence_dir / "uart").glob("tile-*.log")):
        match = re.fullmatch(r"tile-([0-9]+)\.log", path.name)
        _require(match is not None, f"unexpected UART log name: {path}")
        assert match is not None
        tile_id = int(match.group(1))
        text = path.read_text(encoding="utf-8", errors="replace")
        _require(text.count("SCULPTOR_RA_SIM_PASS") == 1, f"tile {tile_id} does not have exactly one pass marker")
        _require(f"SCULPTOR_RA_INIT_PASS tile={tile_id}" in text, f"tile {tile_id} initialization marker is missing")
        movement_lines = [
            line for line in text.splitlines() if line.startswith(MOVEMENT_PREFIX)
        ]
        movement_seen |= bool(movement_lines)
        if movement_lines:
            _require(
                len(movement_lines) == 1,
                f"tile {tile_id} must report exactly one movement summary",
            )
            movement_by_tile[tile_id] = _movement_record(
                movement_lines[0], tile_id
            )
        actual.append(tile_id)
    _require(sorted(actual) == active_tiles, "UART logs do not exactly match active tiles")
    if not movement_seen and not require_movement:
        return None
    _require(
        sorted(movement_by_tile) == active_tiles,
        "movement summaries do not exactly match active tiles",
    )
    totals = {
        name: sum(record[name] for record in movement_by_tile.values())
        for name in MOVEMENT_FIELDS
    }
    return {
        "tile_count": len(movement_by_tile),
        "totals": totals,
        "tiles": {
            str(tile_id): movement_by_tile[tile_id] for tile_id in active_tiles
        },
    }


def _router_statistics(
    evidence_dir: Path, active_tile_count: int, epoch_count: int, idle_arrivals: int
) -> dict[str, Any]:
    rows = _csv_rows(evidence_dir / "router-statistics.csv")
    _require(bool(rows), "router statistics are empty")
    ram = Counter()
    barrier = Counter()
    noc = Counter()
    maximum_sim_time_ps = 0
    for row in rows:
        component = row.get("ComponentName", "")
        metric = row.get("StatisticName", "")
        port = row.get("StatisticSubId", "")
        value = _nonnegative_integer(row.get("Sum.u64"), f"router statistic {component}/{metric}/{port}")
        maximum_sim_time_ps = max(
            maximum_sim_time_ps,
            _nonnegative_integer(row.get("SimTime"), f"router statistic {component}/{metric} SimTime"),
        )
        if component == "global_ram":
            ram[metric] += value
        elif component == "epoch_barrier":
            barrier[metric] += value
        elif component.startswith("router_") and port not in {"local", "port4"}:
            if metric in {
                "flits_forwarded",
                "packets_forwarded",
                "input_buffer_full_cycles",
                "switch_arbitration_stall_cycles",
                "output_credit_stall_cycles",
                "output_link_busy_cycles",
            }:
                noc[metric] += value
    for metric in (
        "requests", "bytes", "readiness_delay_cycles",
        "queue_delay_cycles", "service_cycles",
        "execution_teardown_wait_cycles", "maximum_queue_occupancy",
    ):
        _require(metric in ram, f"global-RAM statistic {metric} is missing")
    for metric in ("arrivals", "idle_arrivals", "releases", "barrier_wait_cycles"):
        _require(metric in barrier, f"epoch-barrier statistic {metric} is missing")
    _require(barrier["arrivals"] == active_tile_count * epoch_count, "router epoch-arrival total does not match")
    _require(barrier["idle_arrivals"] == idle_arrivals, "router idle-arrival total does not match log")
    _require(barrier["releases"] == epoch_count, "router epoch-release total does not match")
    return {
        "maximum_sim_time_ps": maximum_sim_time_ps,
        "global_ram": dict(sorted(ram.items())),
        "epoch_barrier": dict(sorted(barrier.items())),
        "noc_nonlocal": dict(sorted(noc.items())),
    }


def _validate_movement(
    movement: dict[str, Any] | None,
    router: dict[str, Any],
    phase4: dict[str, Any] | None,
) -> dict[str, Any] | None:
    if movement is None:
        _require(
            phase4 is None,
            "complete Phase-4 accounting requires per-tile movement summaries",
        )
        return None
    totals = movement["totals"]
    ram_requests = totals["physical_global_ram_dma_requests"]
    ram_completions = totals["physical_global_ram_dma_completions"]
    ram_bytes = totals["physical_global_ram_dma_bytes"]
    for tile_id, tile in movement["tiles"].items():
        _require(
            tile["physical_global_ram_dma_requests"]
            == tile["physical_global_ram_dma_completions"],
            f"tile {tile_id} physical global-RAM DMA requests and completions "
            "are unbalanced",
        )
    _require(
        ram_requests == ram_completions,
        "physical global-RAM DMA requests and completions are unbalanced",
    )
    _require(
        ram_requests == router["global_ram"]["requests"],
        "movement and router physical global-RAM request counts disagree",
    )
    _require(
        ram_bytes == router["global_ram"]["bytes"],
        "movement and router physical global-RAM byte counts disagree",
    )
    _require(
        totals["physical_noc_frames_sent"]
        == totals["physical_noc_frames_received"],
        "physical NoC sent and received frame counts are unbalanced",
    )
    _require(
        totals["physical_noc_payload_bytes_sent"]
        == totals["physical_noc_payload_bytes_received"],
        "physical NoC sent and received payload bytes are unbalanced",
    )

    result = dict(movement)
    if phase4 is None:
        result["phase4_certificate"] = None
        return result

    compiler = phase4["counters"]
    physical = phase4["physical_global_ram"]
    _require(
        totals["local_copy_transfers"] == 0 and totals["local_copy_bytes"] == 0,
        "Phase-4 retained forwarding performed a local payload copy",
    )
    _require(
        totals["retained_forwarded_logical_transfers"]
        == compiler["retained_local_logical_transfer_count"],
        "runtime and compiler retained logical transfer counts disagree",
    )
    _require(
        totals["retained_forwarded_logical_bytes"]
        == compiler["retained_local_logical_bytes"],
        "runtime and compiler retained logical byte counts disagree",
    )
    _require(
        ram_requests == physical["remaining_requests"],
        "runtime and compiler remaining physical request counts disagree",
    )
    _require(
        ram_bytes == physical["remaining_bytes"],
        "runtime and compiler remaining physical byte counts disagree",
    )
    result["phase4_certificate"] = {
        "status": PASS,
        "compiler": physical,
        "compiler_directions": phase4["directions"],
        "runtime_remaining_requests": ram_requests,
        "runtime_remaining_bytes": ram_bytes,
        "retained_forwarded_logical_transfers": totals[
            "retained_forwarded_logical_transfers"
        ],
        "retained_forwarded_logical_bytes": totals[
            "retained_forwarded_logical_bytes"
        ],
    }
    return result


def _target(simulated_ms: float, lower_bound_ms: float, target_ms: float) -> dict[str, Any]:
    return {
        "target_ms": target_ms,
        "baseline_speedup_required": simulated_ms / target_ms,
        "baseline_reduction_ms_required": max(0.0, simulated_ms - target_ms),
        "baseline_reduction_percent_required": max(0.0, 100.0 * (1.0 - target_ms / simulated_ms)),
        "current_work_lower_bound_ms": lower_bound_ms,
        "verdict": (
            "current_work_lower_bound_exceeds_target"
            if lower_bound_ms >= target_ms
            else "not_ruled_out_by_component_lower_bounds"
        ),
    }


def analyze(compile_dir: Path, evidence_dir: Path) -> dict[str, Any]:
    compile_dir = compile_dir.resolve()
    evidence_dir = evidence_dir.resolve()
    _require(compile_dir.is_dir(), f"compiler directory does not exist: {compile_dir}")
    _require(evidence_dir.is_dir(), f"evidence directory does not exist: {evidence_dir}")

    compile_status = _status(compile_dir / "status.csv", COMPILE_PASS)
    evidence_status = _status(evidence_dir / "status.csv", PASS)
    active_tiles = _active_tiles(compile_dir / "active-cores.txt")
    active_tile_count = len(active_tiles)

    manifest_path = compile_dir / "run-manifest.json"
    manifest = _json(manifest_path)
    _require(manifest.get("schema") == "golem.sculptor-run", "invalid compiler run-manifest schema")
    _require(manifest.get("schema_version") == 1, "unsupported compiler run-manifest version")
    run = manifest.get("run")
    _require(isinstance(run, dict), "compiler run-manifest run object is missing")
    _require(run.get("mode") == "compile", "compiler run-manifest is not compile mode")
    model = run.get("model")
    _require(isinstance(model, str) and model, "compiler run-manifest model is missing")

    hardware = manifest.get("hardware")
    environment = manifest.get("environment")
    architecture_manifest = manifest.get("architecture_manifest")
    _require(isinstance(hardware, dict), "compiler hardware configuration is missing")
    _require(isinstance(environment, dict), "compiler environment is missing")
    _require(isinstance(architecture_manifest, dict), "compiler architecture manifest is missing")
    architecture = architecture_manifest.get("architecture")
    _require(isinstance(architecture, dict), "compiler architecture configuration is missing")
    shard_bytes = _positive_integer(architecture.get("fixed_shard_bytes"), "fixed shard bytes")
    _require(
        shard_bytes in SUPPORTED_MAXIMUM_FRAME_BYTES,
        "residency audit requires a supported maximum frame size from "
        "4 KiB through 256 KiB",
    )
    scratchpad_bytes = _positive_integer(architecture.get("scratchpad_bytes"), "scratchpad bytes")
    global_ram_bytes = _positive_integer(architecture.get("global_ram_bytes"), "global RAM bytes")

    deployment = _json(compile_dir / "deployment-manifest.json")
    _require(deployment.get("schema") == "sculptor.deployment", "invalid deployment schema")
    deployment_version = deployment.get("version")
    _require(deployment_version in {1, 2}, "unsupported deployment version")
    _require(deployment.get("active_tile_ids") == active_tiles, "deployment active tiles do not match")
    if deployment_version == 1:
        epoch_barrier = deployment.get("epoch_barrier")
        _require(
            isinstance(epoch_barrier, dict),
            "deployment epoch-barrier record is missing",
        )
        epoch_count = _positive_integer(
            epoch_barrier.get("epoch_count"), "deployment epoch count"
        )
    else:
        synchronization = deployment.get("synchronization")
        _require(
            isinstance(synchronization, dict),
            "deployment synchronization record is missing",
        )
        _require(
            set(synchronization) == {"mode", "semantic_epoch_count"},
            "deployment synchronization record is malformed",
        )
        _require(
            synchronization.get("mode") == "bulk_barrier",
            "residency feasibility epoch analysis requires bulk_barrier evidence",
        )
        epoch_count = _positive_integer(
            synchronization.get("semantic_epoch_count"),
            "deployment semantic epoch count",
        )

    materialization_payload, materialization = _materialization(
        compile_dir, active_tiles, epoch_count
    )
    phase4 = _phase4_materialization(
        materialization_payload, active_tiles, shard_bytes
    )
    memory = _memory_summary(compile_dir, active_tiles)

    launch_path = evidence_dir / "launch.json"
    launch = _json(launch_path)
    _require(launch.get("schema") == "golem.sculptor-sst-reuse", "invalid SST launch schema")
    _require(launch.get("schema_version") == 1, "unsupported SST launch version")
    _require(launch.get("model") == model, "SST launch model does not match compiler model")
    _require(Path(str(launch.get("source_compile_directory"))).resolve() == compile_dir, "SST launch source directory does not match")
    _require(launch.get("source_run_manifest_sha256") == _sha256(manifest_path), "SST launch compiler-manifest hash does not match")
    materialization_path = compile_dir / "materialization-audit.json"
    _require(launch.get("materialization_audit_sha256") == _sha256(materialization_path), "SST launch materialization hash does not match")
    launch_hardware = launch.get("hardware")
    _require(launch_hardware == hardware, "SST launch hardware does not match compiler hardware")
    global_ram_channels = _positive_integer(launch.get("global_ram_channels"), "global RAM channels")

    result = _single_csv_row(evidence_dir / "result.csv")
    _require(result.get("status") == PASS, "SST result did not pass")
    _require(result.get("model") == model, "SST result model does not match")
    _require(_integer(result.get("active_tiles"), "SST result active_tiles") == active_tile_count, "SST result active-tile count does not match")
    _require(_integer(result.get("epoch_count"), "SST result epoch_count") == epoch_count, "SST result epoch count does not match")
    _require(_integer(result.get("global_ram_channels"), "SST result global_ram_channels") == global_ram_channels, "SST result RAM-channel count does not match")
    _require(
        result.get("output_validation") == "SKIPPED",
        "SST result must explicitly skip numerical output validation",
    )
    simulated_time_text = result.get("simulated_time", "")
    simulated_ms = _time_ms(simulated_time_text)
    _require(simulated_ms > 0.0, "simulated time must be positive")

    epoch_records, _ = _epochs(evidence_dir, active_tile_count, epoch_count)
    movement = _uart_completion(
        evidence_dir, active_tiles, require_movement=phase4 is not None
    )
    performance = _performance(evidence_dir, active_tiles)
    idle_arrivals = sum(row["idle"] for row in epoch_records)
    active_contributions = sum(row["active"] for row in epoch_records)
    router = _router_statistics(
        evidence_dir, active_tile_count, epoch_count, idle_arrivals
    )
    movement = _validate_movement(movement, router, phase4)

    cpu_clock = str(environment.get("GOLEM_MODEL_CPU_CLOCK", ""))
    cpu_hz = _frequency_hz(cpu_clock)
    issue_width = _positive_integer(environment.get("GOLEM_MODEL_CPU_ISSUE_WIDTH"), "CPU issue width")
    arrays_per_core = _positive_integer(hardware.get("arrays_per_core"), "arrays per core")
    mesh_rows = _positive_integer(hardware.get("mesh_rows"), "mesh rows")
    mesh_columns = _positive_integer(hardware.get("mesh_columns"), "mesh columns")

    totals = performance["totals"]
    maximums = performance["maximums"]
    instruction_bound_cycles = math.ceil(
        totals["instructions"] / (active_tile_count * issue_width)
    )
    ram_service_bound_cycles = math.ceil(
        router["global_ram"]["service_cycles"] / global_ram_channels
    )
    analog_balance_cycles = math.ceil(
        totals["analog_active_cycles"] / active_tile_count
    )
    current_work_bound_cycles = max(instruction_bound_cycles, ram_service_bound_cycles)
    cycles_to_ms = 1000.0 / cpu_hz
    current_work_bound_ms = current_work_bound_cycles * cycles_to_ms

    capacity = memory["summaries"]["capacity"]["maximums"]
    finalized = memory["summaries"]["finalized"]["maximums"]
    physical = memory["summaries"]["physical"]["maximums"]
    per_tile_physical = [
        memory["tiles"][str(tile_id)].get("physical", {})
        for tile_id in active_tiles
    ]
    _require(
        all(isinstance(record, dict) for record in per_tile_physical),
        "one or more per-tile physical memory summaries are missing",
    )
    required_local_bytes = _nonnegative_integer(capacity.get("requiredLocalBytes"), "maximum required local bytes")
    planned_scratchpad_bytes = _nonnegative_integer(capacity.get("scratchpadBytes"), "maximum planned scratchpad bytes")
    persistent_bytes = _nonnegative_integer(capacity.get("persistentBytes"), "maximum persistent bytes")
    route_input_bytes = _nonnegative_integer(finalized.get("route_input_bytes"), "maximum route-input bytes")
    route_output_bytes = _nonnegative_integer(finalized.get("route_output_bytes"), "maximum route-output bytes")
    aggregate_spm_bytes = scratchpad_bytes * active_tile_count
    maximum_live_global_bytes = materialization["maximum_live_global_ram_bytes"]

    histogram = Counter(row["active"] for row in epoch_records)
    interval_records = [
        {
            "epoch": row["completed"],
            "active_tiles": row["active"],
            "idle_tiles": row["idle"],
            "first_arrival_cycle": row["first"],
            "release_cycle": row["release"],
            "interval_cycles": row["interval_cycles"],
        }
        for row in epoch_records
    ]
    longest = sorted(
        interval_records,
        key=lambda row: (-row["interval_cycles"], row["epoch"]),
    )[:20]

    validation_checks = [
        "compile_manifest_and_status",
        "active_tiles_and_deployment",
        "materialization_ownership_and_accounting",
        "tile_memory_accounting",
        "sst_launch_provenance",
        "all_tile_initialization_and_completion",
        "all_epoch_releases_and_arrivals",
        "router_and_barrier_accounting",
        "performance_summary_coverage",
        "supported_configurable_physical_frames",
        "numerical_output_validation_explicitly_skipped",
    ]
    if movement is not None:
        validation_checks.extend(
            [
                "one_movement_summary_per_tile",
                "runtime_physical_movement_balance",
            ]
        )
    if phase4 is not None:
        validation_checks.extend(
            [
                "phase4_before_remaining_elided_physical_certificate",
                "phase4_compiler_runtime_physical_movement_join",
                "phase4_zero_copy_retained_forwarding",
            ]
        )

    payload = {
        "schema": CERTIFICATE_SCHEMA,
        "schema_version": CERTIFICATE_VERSION,
        "status": PASS,
        "model": model,
        "provenance": {
            "compile_directory": str(compile_dir),
            "evidence_directory": str(evidence_dir),
            "compiler_run_manifest_sha256": _sha256(manifest_path),
            "materialization_audit_sha256": _sha256(materialization_path),
            "deployment_manifest_sha256": _sha256(compile_dir / "deployment-manifest.json"),
            "sst_launch_manifest_sha256": _sha256(launch_path),
            "router_statistics_sha256": _sha256(evidence_dir / "router-statistics.csv"),
        },
        "validation": {
            "status": PASS,
            "model_outputs_checked": False,
            "model_output_validation_required": False,
            "compile_status": compile_status,
            "sst_status": evidence_status,
            "checks": validation_checks,
        },
        "configuration": {
            "mesh_rows": mesh_rows,
            "mesh_columns": mesh_columns,
            "active_tiles": active_tile_count,
            "active_tile_ids": active_tiles,
            "arrays_per_core": arrays_per_core,
            "cpu_clock": cpu_clock,
            "cpu_issue_width": issue_width,
            "fixed_shard_bytes": shard_bytes,
            "scratchpad_bytes_per_tile": scratchpad_bytes,
            "global_ram_bytes": global_ram_bytes,
            "global_ram_channels": global_ram_channels,
            "digital_workers": _positive_integer(environment.get("GOLEM_MODEL_DIGITAL_WORKERS"), "digital workers"),
        },
        "completion": {
            "active_tiles_completed": active_tile_count,
            "active_tiles_expected": active_tile_count,
            "epoch_releases_completed": epoch_count,
            "epoch_releases_expected": epoch_count,
            "final_epoch_release_cycle": epoch_records[-1]["release"],
            "simulated_time": simulated_time_text,
            "simulated_time_ms": simulated_ms,
            "simulator_time_ps": router["maximum_sim_time_ps"],
        },
        "materialization": materialization,
        "phase4_materialization": phase4,
        "scratchpad_feasibility": {
            "per_tile_capacity_bytes": scratchpad_bytes,
            "aggregate_active_capacity_bytes": aggregate_spm_bytes,
            "maximum_required_local_bytes": required_local_bytes,
            "maximum_planned_scratchpad_bytes": planned_scratchpad_bytes,
            "maximum_persistent_bytes": persistent_bytes,
            "maximum_route_input_bytes": route_input_bytes,
            "maximum_route_output_bytes": route_output_bytes,
            "maximum_live_global_ram_bytes": maximum_live_global_bytes,
            "maximum_live_global_fits_aggregate_spm": maximum_live_global_bytes <= aggregate_spm_bytes,
            "maximum_live_global_to_aggregate_spm_ratio": maximum_live_global_bytes / aggregate_spm_bytes,
            "maximum_planned_to_per_tile_spm_ratio": planned_scratchpad_bytes / scratchpad_bytes,
            "aggregate_headroom_if_perfectly_distributed_bytes": aggregate_spm_bytes - maximum_live_global_bytes,
            "maximum_physical_static_image_bytes": _nonnegative_integer(physical.get("static_image_bytes"), "maximum physical static image"),
            "minimum_estimated_heap_headroom_bytes": min(
                _nonnegative_integer(
                    record.get("estimated_heap_headroom_bytes"),
                    "per-tile estimated heap headroom",
                )
                for record in per_tile_physical
            ),
        },
        "epoch_parallelism": {
            "epoch_count": epoch_count,
            "available_tile_epochs": active_tile_count * epoch_count,
            "working_tile_epochs": active_contributions,
            "idle_tile_epochs": idle_arrivals,
            "mean_working_tiles_per_epoch": active_contributions / epoch_count,
            "tile_epoch_utilization": active_contributions / (active_tile_count * epoch_count),
            "working_tile_histogram": {str(key): histogram[key] for key in sorted(histogram)},
            "longest_intervals": longest,
            "intervals": interval_records,
        },
        "work": {
            "performance_tile_count": performance["tile_count"],
            "totals": totals,
            "per_tile_maximums": maximums,
        },
        "fabric": {
            "global_ram": router["global_ram"],
            "epoch_barrier": router["epoch_barrier"],
            "noc_nonlocal": router["noc_nonlocal"],
            "movement": movement,
        },
        "component_lower_bounds": {
            "scope": "current_work_unchanged_with_perfect_balance_and_overlap",
            "cpu_instruction_issue": {
                "cycles": instruction_bound_cycles,
                "milliseconds": instruction_bound_cycles * cycles_to_ms,
            },
            "global_ram_channel_service": {
                "cycles": ram_service_bound_cycles,
                "milliseconds": ram_service_bound_cycles * cycles_to_ms,
            },
            "analog_active_tile_balance": {
                "cycles": analog_balance_cycles,
                "milliseconds": analog_balance_cycles * cycles_to_ms,
                "diagnostic_only": True,
            },
            "maximum_current_work_hard_bound": {
                "cycles": current_work_bound_cycles,
                "milliseconds": current_work_bound_ms,
            },
        },
        "targets": {
            "primary_sub_10ms": _target(simulated_ms, current_work_bound_ms, 10.0),
            "stretch_sub_1ms": _target(simulated_ms, current_work_bound_ms, 1.0),
        },
        "verdict": {
            "aggregate_spm_can_hold_maximum_live_global_working_set": maximum_live_global_bytes <= aggregate_spm_bytes,
            "global_ram_channels_saturated": router["global_ram"]["maximum_queue_occupancy"] >= global_ram_channels,
            "epoch_parallelism_is_primary_observed_gap": active_contributions < (active_tile_count * epoch_count) // 2,
            "primary_target": _target(simulated_ms, current_work_bound_ms, 10.0)["verdict"],
            "stretch_target": _target(simulated_ms, current_work_bound_ms, 1.0)["verdict"],
            "interpretation": (
                "The current execution is dominated by sparse epoch scheduling and barrier stragglers, "
                "not aggregate SPM capacity or global-RAM channel occupancy. The primary target is not "
                "ruled out by the component lower bounds. The stretch target requires reducing the "
                "current instruction work in addition to balancing it."
            ),
        },
    }
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-dir", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        payload = analyze(args.compile_dir, args.evidence_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (AuditError, OSError, ValueError) as error:
        print(f"residency feasibility audit failed: {error}", file=sys.stderr)
        return 1
    print(
        "residency feasibility audit: PASS "
        f"model={payload['model']} "
        f"tiles={payload['completion']['active_tiles_completed']} "
        f"epochs={payload['completion']['epoch_releases_completed']} "
        f"simulated_ms={payload['completion']['simulated_time_ms']:.6f} "
        "outputs_checked=false"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
