#!/usr/bin/env python3

"""Classify a live or completed Sculptor SST evidence directory.

The classifier deliberately distinguishes architectural progress from mere
guest execution and distinguishes an unavailable counter from a real zero.
It is safe to run while SST is appending the CSV and UART files.
"""

import argparse
import csv
import hashlib
import json
import os
import re
import time
from collections import Counter
from pathlib import Path


INVALID_U32 = (1 << 32) - 1
TILE_PATTERN = re.compile(r"tile-([0-9]+)-progress[.]csv$")
UART_TILE_PATTERN = re.compile(r"tile-([0-9]+)[.]log$")
INIT_PATTERN = re.compile(r"SCULPTOR_RA_INIT_PASS tile=([0-9]+)")
PASS_PATTERN = re.compile(
    r"SCULPTOR_RA_SIM_PASS(?: tile=([0-9]+))?(?:\s|$)"
)
ERROR_PATTERN = re.compile(r"SCULPTOR_RA_SIM_ERROR(?: |$)")
RUNTIME_PROGRESS_PREFIX = "SCULPTOR_RA_PROGRESS "

# These counters represent an architectural state transition.  Instruction,
# simulation-tick, synchronization-event, and wait-tick deltas are reported
# separately because a guest can advance all of them while spinning.
ARCHITECTURAL_COUNTERS = (
    "task_finish_events",
    "physical_global_dma_submitted",
    "physical_global_dma_completed",
    "analog_commands_submitted",
    "analog_commands_completed",
    "network_packets",
    "network_words",
    "receive_dma_transfers",
)
ACTIVITY_COUNTERS = (
    "simulation_tick",
    "instructions",
    "cpu_cycles",
    "synchronization_events",
    "synchronization_grants",
)
PENDING_COUNTERS = (
    "pending_network_receives",
    "pending_receive_dma",
    "pending_receive_dma_descriptors",
    "pending_completed_receive_frames",
    "pending_ready_receive_bursts",
    "pending_incoming_frame_assemblies",
    "bridge_receive_bursts",
    "pending_global_dma",
    "pending_memory",
)
REQUIRED_COUNTERS = tuple(
    dict.fromkeys(ACTIVITY_COUNTERS + ARCHITECTURAL_COUNTERS + PENDING_COUNTERS)
)


class EvidenceError(RuntimeError):
    pass


def parse_unsigned(value, field, path):
    try:
        result = int(value)
    except (TypeError, ValueError) as error:
        raise EvidenceError(f"{path}: invalid {field}: {value!r}") from error
    if result < 0:
        raise EvidenceError(f"{path}: negative {field}: {result}")
    return result


def physical_dma_certificate_from_payload(payload, source):
    if not isinstance(payload, dict):
        raise EvidenceError(f"{source}: physical DMA work certificate must be an object")
    required = ("input_requests", "output_requests", "total_requests")
    values = {
        name: parse_unsigned(payload.get(name), name, source) for name in required
    }
    if values["input_requests"] + values["output_requests"] != values["total_requests"]:
        raise EvidenceError(
            f"{source}: physical DMA input/output request counts do not sum to total"
        )

    raw_tiles = payload.get("tile_requests")
    tile_requests = None
    if raw_tiles is not None:
        if not isinstance(raw_tiles, dict):
            raise EvidenceError(f"{source}: tile_requests must be an object")
        tile_requests = {}
        for raw_tile, raw_count in raw_tiles.items():
            try:
                tile = int(raw_tile)
            except (TypeError, ValueError) as error:
                raise EvidenceError(
                    f"{source}: invalid tile_requests key {raw_tile!r}"
                ) from error
            if tile < 0 or str(tile) != str(raw_tile):
                raise EvidenceError(
                    f"{source}: tile_requests keys must be canonical unsigned integers"
                )
            if tile in tile_requests:
                raise EvidenceError(f"{source}: duplicate tile request count for {tile}")
            tile_requests[tile] = parse_unsigned(
                raw_count, f"tile_requests[{tile}]", source
            )
        if sum(tile_requests.values()) != values["total_requests"]:
            raise EvidenceError(
                f"{source}: per-tile physical DMA requests do not sum to total"
            )
    return {**values, "tile_requests": tile_requests}


def physical_dma_certificate_from_audit(path):
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read materialization audit {path}: {error}") from error
    if not isinstance(payload, dict):
        raise EvidenceError(f"{path}: materialization audit must be an object")
    counters = payload.get("counters")
    tiles = payload.get("tiles")
    if not isinstance(counters, dict) or not isinstance(tiles, list):
        raise EvidenceError(f"{path}: materialization audit lacks counters or tiles")
    input_requests = parse_unsigned(
        counters.get("materialized_input_physical_request_count"),
        "materialized_input_physical_request_count",
        path,
    )
    output_requests = parse_unsigned(
        counters.get("materialized_output_physical_request_count"),
        "materialized_output_physical_request_count",
        path,
    )
    tile_requests = {}
    for index, tile_payload in enumerate(tiles):
        if not isinstance(tile_payload, dict):
            raise EvidenceError(f"{path}: tiles[{index}] must be an object")
        tile = parse_unsigned(tile_payload.get("tile_id"), "tile_id", path)
        if tile in tile_requests:
            raise EvidenceError(f"{path}: duplicate tile {tile} in materialization audit")
        tile_input = parse_unsigned(
            tile_payload.get("materialized_input_physical_request_count"),
            f"tiles[{index}].materialized_input_physical_request_count",
            path,
        )
        tile_output = parse_unsigned(
            tile_payload.get("materialized_output_physical_request_count"),
            f"tiles[{index}].materialized_output_physical_request_count",
            path,
        )
        tile_requests[tile] = tile_input + tile_output
    return physical_dma_certificate_from_payload(
        {
            "input_requests": input_requests,
            "output_requests": output_requests,
            "total_requests": input_requests + output_requests,
            "tile_requests": {str(tile): count for tile, count in tile_requests.items()},
        },
        path,
    )


def read_physical_dma_certificate(launch):
    embedded = launch.get("physical_global_dma_work")
    embedded_certificate = (
        physical_dma_certificate_from_payload(embedded, "launch.json")
        if embedded is not None
        else None
    )

    source = launch.get("source_compile_directory")
    expected_hash = launch.get("materialization_audit_sha256")
    if not isinstance(source, str) or not source or not isinstance(expected_hash, str):
        return embedded_certificate, "launch" if embedded_certificate is not None else None
    path = Path(source) / "materialization-audit.json"
    if not path.is_file():
        raise EvidenceError(f"hash-bound materialization audit is missing: {path}")
    observed_hash = hashlib.sha256(path.read_bytes()).hexdigest()
    if observed_hash != expected_hash:
        raise EvidenceError(
            f"{path}: materialization audit hash {observed_hash} does not match launch {expected_hash}"
        )
    audit_certificate = physical_dma_certificate_from_audit(path)
    if embedded_certificate is not None and embedded_certificate != audit_certificate:
        raise EvidenceError(
            "launch physical DMA work certificate disagrees with its hash-bound "
            "materialization audit"
        )
    if embedded_certificate is not None:
        return embedded_certificate, "launch_and_hash_bound_materialization_audit"
    return audit_certificate, "hash_bound_materialization_audit"


def read_key_values(path):
    values = {}
    if not path.is_file():
        return values
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for raw in stream:
            line = raw.strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key] = value
    return values


def read_launch(evidence):
    payload = {}
    path = evidence / "launch.json"
    if path.is_file():
        with path.open("r", encoding="utf-8") as stream:
            payload = json.load(stream)
        if not isinstance(payload, dict):
            raise EvidenceError(f"{path}: launch metadata must be an object")
    text = read_key_values(evidence / "launch.txt")
    if "active_tiles" in text:
        payload["active_tiles"] = parse_unsigned(
            text["active_tiles"], "active_tiles", evidence / "launch.txt"
        )
    if "guest_task_trace_enabled" in text:
        value = parse_unsigned(
            text["guest_task_trace_enabled"],
            "guest_task_trace_enabled",
            evidence / "launch.txt",
        )
        if value not in (0, 1):
            raise EvidenceError("guest_task_trace_enabled must be zero or one")
        payload["guest_task_trace_enabled"] = value == 1
    return payload


def frozen_guest_task_trace_enabled(launch):
    configured = launch.get("guest_task_trace_enabled")
    if isinstance(configured, bool):
        return configured
    source = launch.get("source_compile_directory")
    if isinstance(source, str) and source:
        manifest_path = Path(source) / "run-manifest.json"
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise EvidenceError(
                    f"cannot read frozen source manifest {manifest_path}: {error}"
                ) from error
            mode = manifest.get("environment", {}).get("GOLEM_MODEL_PROFILE_MODE")
            if mode in ("off", "summary", "trace"):
                return mode == "trace"
    # Old evidence did not separate frozen guest instrumentation from host
    # profile collection. This fallback preserves its historical behavior.
    return launch.get("profile_mode") == "trace"


def valid_progress_rows(path):
    rows = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    reader = csv.DictReader(lines)
    if reader.fieldnames is None:
        return rows
    missing = set(REQUIRED_COUNTERS) - set(reader.fieldnames)
    if missing:
        names = ", ".join(sorted(missing))
        raise EvidenceError(f"{path}: missing progress columns: {names}")
    raw_rows = list(reader)
    for index, raw in enumerate(raw_rows):
        final_record = index + 1 == len(raw_rows)
        # A concurrent writer can leave one partial trailing row.  Reject it
        # as a sample without discarding the preceding committed row.
        incomplete = None in raw or any(
            raw.get(name) in (None, "") for name in REQUIRED_COUNTERS
        )
        if incomplete:
            if final_record:
                continue
            raise EvidenceError(f"{path}: incomplete interior progress row")
        try:
            row = {
                name: parse_unsigned(raw[name], name, path)
                for name in REQUIRED_COUNTERS
            }
            row["tile_id"] = parse_unsigned(raw["tile_id"], "tile_id", path)
            row["wait_reason"] = parse_unsigned(
                raw.get("wait_reason", "0"), "wait_reason", path
            )
            row["local_epoch"] = parse_unsigned(
                raw.get("local_epoch", str(INVALID_U32)), "local_epoch", path
            )
            for name in (
                "task_finish_events_available",
                "local_epoch_available",
                "memory_initialization_complete",
            ):
                if name in raw and raw[name] not in (None, ""):
                    row[name] = parse_unsigned(raw[name], name, path)
        except EvidenceError:
            if final_record:
                continue
            raise
        rows.append(row)
    return rows


def parse_runtime_progress(line):
    if not line.startswith(RUNTIME_PROGRESS_PREFIX):
        return None
    fields = {}
    for token in line[len(RUNTIME_PROGRESS_PREFIX) :].split():
        if "=" not in token:
            continue
        name, value = token.split("=", 1)
        if value.isdigit():
            fields[name] = int(value)
    return fields if "tile" in fields else None


def read_uart(evidence):
    init_tiles = set()
    pass_tiles = set()
    error_tiles = set()
    runtime_progress = {}
    uart_dir = evidence / "uart"
    if not uart_dir.is_dir():
        return init_tiles, pass_tiles, error_tiles, runtime_progress
    for path in sorted(uart_dir.glob("tile-*.log")):
        match = UART_TILE_PATTERN.search(path.name)
        if match is None:
            continue
        file_tile = int(match.group(1))
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                init = INIT_PATTERN.search(line)
                if init is not None:
                    init_tiles.add(int(init.group(1)))
                passed = PASS_PATTERN.search(line)
                if passed is not None:
                    pass_tiles.add(
                        int(passed.group(1))
                        if passed.group(1) is not None
                        else file_tile
                    )
                if ERROR_PATTERN.search(line) is not None:
                    error_tiles.add(file_tile)
                progress = parse_runtime_progress(line.strip())
                if progress is not None:
                    runtime_progress[progress["tile"]] = progress
    return init_tiles, pass_tiles, error_tiles, runtime_progress


def availability(row, name, inferred):
    value = row.get(name)
    if value is None:
        return inferred
    if value not in (0, 1):
        raise EvidenceError(f"invalid Boolean availability value {name}={value}")
    return value == 1


def classify(evidence, stale_after_seconds):
    launch = read_launch(evidence)
    profile_mode = launch.get("profile_mode")
    synchronization_mode = launch.get("synchronization_mode")
    guest_task_trace_enabled = frozen_guest_task_trace_enabled(launch)
    progress_dir = evidence / "trace" / "performance"
    paths = sorted(progress_dir.glob("tile-*-progress.csv"))
    if not paths:
        raise EvidenceError(f"no tile progress files under {progress_dir}")

    now = time.time()
    samples = {}
    stale_tiles = []
    for path in paths:
        match = TILE_PATTERN.search(path.name)
        if match is None:
            continue
        tile = int(match.group(1))
        rows = valid_progress_rows(path)
        if not rows:
            raise EvidenceError(f"{path}: no complete progress rows")
        if rows[-1]["tile_id"] != tile:
            raise EvidenceError(
                f"{path}: filename tile {tile} disagrees with row tile {rows[-1]['tile_id']}"
            )
        samples[tile] = rows[-2:] if len(rows) >= 2 else rows
        if now - path.stat().st_mtime > stale_after_seconds:
            stale_tiles.append(tile)

    expected_tiles = launch.get("active_tiles", len(samples))
    if not isinstance(expected_tiles, int) or expected_tiles <= 0:
        raise EvidenceError("active_tiles must be a positive integer")

    init_tiles, pass_tiles, error_tiles, runtime_progress = read_uart(evidence)
    deltas = Counter()
    totals = Counter()
    pending = Counter()
    wait_reasons = Counter()
    regressions = []
    one_sample_tiles = []
    task_available_tiles = 0
    epoch_available_tiles = 0
    host_epochs = []
    memory_initialized_tiles = 0
    tile_latest = {}

    inferred_task_available = guest_task_trace_enabled
    inferred_epoch_available = synchronization_mode not in (None, "exact_dependencies")
    stale_tile_set = set(stale_tiles)
    for tile, rows in sorted(samples.items()):
        latest = rows[-1]
        tile_latest[tile] = latest
        task_available = availability(
            latest, "task_finish_events_available", inferred_task_available
        )
        if len(rows) == 1:
            one_sample_tiles.append(tile)
        else:
            previous = rows[-2]
            for name in ACTIVITY_COUNTERS + ARCHITECTURAL_COUNTERS:
                delta = latest[name] - previous[name]
                if delta < 0:
                    regressions.append({"tile": tile, "counter": name, "delta": delta})
                elif tile not in stale_tile_set and (
                    name != "task_finish_events" or task_available
                ):
                    deltas[name] += delta
        for name in ACTIVITY_COUNTERS + ARCHITECTURAL_COUNTERS:
            totals[name] += latest[name]
        for name in PENDING_COUNTERS:
            pending[name] += latest[name]
        wait_reasons[latest["wait_reason"]] += 1

        if task_available:
            task_available_tiles += 1
        epoch_available = availability(
            latest, "local_epoch_available", inferred_epoch_available
        )
        if epoch_available:
            if latest["local_epoch"] == INVALID_U32:
                raise EvidenceError(
                    f"tile {tile}: local epoch is marked available but is invalid"
                )
            epoch_available_tiles += 1
            host_epochs.append(latest["local_epoch"])
        elif latest["local_epoch"] != INVALID_U32:
            raise EvidenceError(
                f"tile {tile}: unavailable local epoch must use UINT32_MAX"
            )
        initialized = latest.get("memory_initialization_complete")
        if initialized is None:
            initialized = 1 if tile in init_tiles else 0
        if initialized not in (0, 1):
            raise EvidenceError(
                f"tile {tile}: invalid memory_initialization_complete={initialized}"
            )
        memory_initialized_tiles += initialized

    dma_certificate, dma_certificate_source = read_physical_dma_certificate(launch)
    dma_accounting_violations = []
    if dma_certificate is not None:
        expected_by_tile = dma_certificate["tile_requests"]
        if expected_by_tile is not None:
            unexpected = sorted(set(samples) - set(expected_by_tile))
            missing = sorted(set(expected_by_tile) - set(samples))
            if unexpected or (len(samples) == expected_tiles and missing):
                raise EvidenceError(
                    "physical DMA certificate tile set disagrees with progress "
                    f"evidence: missing={missing} unexpected={unexpected}"
                )
            for tile, latest in sorted(tile_latest.items()):
                expected = expected_by_tile[tile]
                submitted = latest["physical_global_dma_submitted"]
                completed = latest["physical_global_dma_completed"]
                if submitted > expected or completed > expected:
                    dma_accounting_violations.append(
                        {
                            "tile": tile,
                            "expected": expected,
                            "submitted": submitted,
                            "completed": completed,
                        }
                    )

    if regressions or dma_accounting_violations:
        state = "invalid_evidence"
    elif error_tiles:
        state = "failed"
    elif len(samples) == expected_tiles and pass_tiles == set(samples):
        state = "passed"
    elif len(samples) != expected_tiles:
        state = "incomplete_evidence"
    elif len(samples) == len(stale_tiles):
        state = "stale_evidence"
    else:
        architectural_delta = sum(deltas[name] for name in ARCHITECTURAL_COUNTERS)
        activity_delta = sum(deltas[name] for name in ACTIVITY_COUNTERS)
        if architectural_delta > 0:
            state = "active_architectural_progress"
        elif activity_delta > 0:
            state = "executing_without_observed_architectural_progress"
        elif one_sample_tiles:
            state = "insufficient_history"
        else:
            state = "indeterminate_stall"

    guest_tasks = [
        value["tasks_retired"]
        for value in runtime_progress.values()
        if "tasks_retired" in value
    ]
    guest_epochs = [
        value["current_epoch"]
        for value in runtime_progress.values()
        if "current_epoch" in value and value["current_epoch"] != INVALID_U32
    ]
    if dma_certificate is None:
        physical_dma_work = {
            "available": False,
            "source": None,
            "input_requests": None,
            "output_requests": None,
            "expected_requests": None,
            "submitted_requests": totals["physical_global_dma_submitted"],
            "completed_requests": totals["physical_global_dma_completed"],
            "remaining_requests": None,
            "completion_fraction": None,
            "completion_percent": None,
            "tiles_at_expected_completion": None,
            "accounting_violations": [],
        }
    else:
        expected = dma_certificate["total_requests"]
        completed = totals["physical_global_dma_completed"]
        expected_by_tile = dma_certificate["tile_requests"]
        physical_dma_work = {
            "available": True,
            "source": dma_certificate_source,
            "input_requests": dma_certificate["input_requests"],
            "output_requests": dma_certificate["output_requests"],
            "expected_requests": expected,
            "submitted_requests": totals["physical_global_dma_submitted"],
            "completed_requests": completed,
            "remaining_requests": max(0, expected - completed),
            "completion_fraction": completed / expected if expected else 1.0,
            "completion_percent": round(
                100.0 * completed / expected if expected else 100.0, 6
            ),
            "tiles_at_expected_completion": (
                sum(
                    tile_latest[tile]["physical_global_dma_completed"] == count
                    for tile, count in expected_by_tile.items()
                    if tile in tile_latest
                )
                if expected_by_tile is not None
                else None
            ),
            "accounting_violations": dma_accounting_violations,
        }
    result = {
        "schema": "golem.sculptor-sst-progress-classification",
        "schema_version": 1,
        "evidence_directory": str(evidence.resolve()),
        "state": state,
        "expected_tiles": expected_tiles,
        "observed_progress_tiles": len(samples),
        "fresh_progress_tiles": len(samples) - len(stale_tiles),
        "stale_progress_tiles": stale_tiles,
        "initialization_pass_tiles": len(init_tiles),
        "memory_initialization_complete_tiles": memory_initialized_tiles,
        "simulation_pass_tiles": len(pass_tiles),
        "simulation_error_tiles": sorted(error_tiles),
        "profile_mode": profile_mode,
        "guest_task_trace_enabled": guest_task_trace_enabled,
        "synchronization_mode": synchronization_mode,
        "deltas": {
            name: deltas[name]
            for name in ACTIVITY_COUNTERS + ARCHITECTURAL_COUNTERS
        },
        "totals": {
            name: totals[name]
            for name in ACTIVITY_COUNTERS + ARCHITECTURAL_COUNTERS
        },
        "pending": {name: pending[name] for name in PENDING_COUNTERS},
        "wait_reason_tile_counts": {
            str(name): count for name, count in sorted(wait_reasons.items())
        },
        "task_completion": {
            "available_tiles": task_available_tiles,
            "value": totals["task_finish_events"]
            if task_available_tiles == len(samples)
            else None,
            "reason": None
            if task_available_tiles == len(samples)
            else "task boundary events have not been observed in this evidence",
        },
        "host_epoch": {
            "available_tiles": epoch_available_tiles,
            "minimum": min(host_epochs) if host_epochs else None,
            "maximum": max(host_epochs) if host_epochs else None,
            "reason": None
            if epoch_available_tiles == len(samples)
            else "exact-dependency mode has no host epoch barrier",
        },
        "guest_runtime_progress": {
            "snapshot_tiles": len(runtime_progress),
            "tasks_retired_sum": sum(guest_tasks) if guest_tasks else None,
            "current_epoch_minimum": min(guest_epochs) if guest_epochs else None,
            "current_epoch_maximum": max(guest_epochs) if guest_epochs else None,
        },
        "physical_global_dma_work": physical_dma_work,
        "counter_regressions": regressions,
        "one_sample_tiles": one_sample_tiles,
        "deadlock_proven": False,
    }
    return result


def render_text(result):
    delta = result["deltas"]
    task = result["task_completion"]
    epoch = result["host_epoch"]
    dma_work = result["physical_global_dma_work"]
    lines = [
        f"state={result['state']}",
        (
            "tiles="
            f"{result['observed_progress_tiles']}/{result['expected_tiles']} "
            f"fresh={result['fresh_progress_tiles']} "
            f"init={result['initialization_pass_tiles']} "
            f"pass={result['simulation_pass_tiles']} "
            f"errors={len(result['simulation_error_tiles'])}"
        ),
        (
            "delta "
            f"instructions={delta['instructions']} "
            f"dma_completed={delta['physical_global_dma_completed']} "
            f"analog_completed={delta['analog_commands_completed']} "
            f"network_packets={delta['network_packets']}"
        ),
        (
            "task_completion="
            + (str(task["value"]) if task["value"] is not None else "unavailable")
        ),
        (
            "host_epoch="
            + (
                f"{epoch['minimum']}..{epoch['maximum']}"
                if epoch["minimum"] is not None
                else "unavailable"
            )
        ),
        (
            "physical_dma_work="
            + (
                f"{dma_work['completed_requests']}/"
                f"{dma_work['expected_requests']} "
                f"({dma_work['completion_percent']:.6f}%) "
                f"remaining={dma_work['remaining_requests']}"
                if dma_work["available"]
                else "unavailable"
            )
        ),
        "deadlock_proven=false",
    ]
    return "\n".join(lines)


def write_json_atomic(path, result):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_directory", type=Path)
    parser.add_argument(
        "--stale-after-seconds",
        type=float,
        default=30.0,
        help="mark a progress file stale after this wall-time interval",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument(
        "--output",
        type=Path,
        help="atomically write the JSON classification to this path",
    )
    arguments = parser.parse_args()
    if arguments.stale_after_seconds <= 0:
        parser.error("--stale-after-seconds must be positive")
    try:
        result = classify(arguments.evidence_directory, arguments.stale_after_seconds)
    except (EvidenceError, OSError, json.JSONDecodeError) as error:
        parser.exit(2, f"progress classification failed: {error}\n")
    if arguments.output is not None:
        if arguments.format != "json":
            parser.error("--output requires --format json")
        try:
            write_json_atomic(arguments.output, result)
        except OSError as error:
            parser.exit(2, f"could not write classification: {error}\n")
    elif arguments.format == "json":
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(render_text(result))


if __name__ == "__main__":
    main()
