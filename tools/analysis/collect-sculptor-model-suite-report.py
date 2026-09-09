#!/usr/bin/env python3
"""Collect the V1 model-suite result and accounting report.

The model harness deliberately keeps the compiler, simulator, and profiler
artifacts in the model run directory.  This collector joins those artifacts
without rerunning a compiler or simulation.  Missing artifacts are represented
as ``NA`` in CSV and ``null`` in JSON so a partial run cannot be mistaken for
zero architectural work.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


REQUIRED_MODELS = (
    "resnet32",
    "wide-resnet-16-8",
    "resnet50",
    "yolov9-c",
    "retinanet",
    "rnnt",
    "mobilebert",
    "bert-large",
)

NA = "NA"
PASS = "PASS"
COMPILE_PASS = "COMPILE_PASS"
FAIL = "FAIL"
NOT_RUN = "NOT_RUN"
NOT_REACHED = "NOT_REACHED"
INCOMPLETE = "INCOMPLETE"
TIMEOUT = "TIMEOUT"
INVALID = "INVALID"

FEATURE_NAMES = (
    "residency",
    "direct_forwarding",
    "persistent_matrices",
    "circular_double_buffering",
    "digital_workers",
    "exact_dependencies_local_epoch",
    "configurable_frames",
)

MATERIALIZATION_COUNTER_FIELDS = (
    "epoch_count",
    "materialized_tensor_count",
    "materialized_tensor_bytes",
    "materialized_producer_region_count",
    "materialized_consumer_region_count",
    "materialized_output_dma_descriptor_count",
    "materialized_input_dma_descriptor_count",
    "materialized_main_transfer_count_logical",
    "materialized_tail_transfer_count_logical",
    "unowned_materialized_byte_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "cross_epoch_direct_route_count",
    "unclassified_boundary_count",
    "maximum_live_global_ram_bytes",
)


def _number(value: str | None, default: int | float | None = None) -> int | float | None:
    if value is None or value.strip() == "":
        return default
    try:
        if any(character in value for character in ".eE"):
            return float(value)
        return int(value)
    except ValueError:
        return default


def _csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open("r", encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source))


def _last_csv_row(path: Path) -> dict[str, str]:
    rows = _csv_rows(path)
    return rows[-1] if rows else {}


def _sum_stage_timings(path: Path) -> float | None:
    rows = _csv_rows(path)
    if not rows:
        return None
    # These are all stages that execute before the simulator is started.  A
    # result-validation stage is intentionally excluded: it is post-sim work,
    # not compiler wall time.
    compiler_stages = {
        "model_export",
        "runtime_build",
        "compiler_lowering",
        "tile_object_codegen",
        "materialization_audit",
        "platform_codegen",
        "abi_preflight",
        "tile_link",
        "memory_validation",
        "compile_qualification",
        "run_manifest",
    }
    values = [
        float(row["wall_seconds"])
        for row in rows
        if row.get("stage") in compiler_stages
        and _number(row.get("wall_seconds")) is not None
    ]
    return sum(values) if values else None


def _resource_usage(path: Path) -> tuple[float | None, int | None]:
    if not path.is_file():
        return None, None
    line = path.read_text(encoding="utf-8").strip().splitlines()
    if not line:
        return None, None
    fields = line[-1].split(",")
    if len(fields) != 2:
        return None, None
    wall = _number(fields[0])
    rss = _number(fields[1])
    return (float(wall) if wall is not None else None,
            int(rss) if rss is not None else None)


def _harness_metrics(run_directory: Path, model: str) -> dict[str, Any]:
    row = _last_csv_row(run_directory / "harness-status.csv")
    empty = {
        "harness_wrapper_status": NOT_RUN,
        "hard_wall_status": NOT_RUN,
        "hard_wall_timeout_seconds": None,
        "harness_wall_seconds": None,
        "harness_exit_code": None,
        "harness_failure_stage": "",
    }
    if not row:
        return empty
    required = {
        "model",
        "run_mode",
        "wrapper_status",
        "command_exit_code",
        "failure_stage",
        "wall_seconds",
        "timeout_seconds",
    }
    if set(row) != required:
        return {**empty, "harness_wrapper_status": INVALID,
                "hard_wall_status": INVALID}
    exit_code = _number(row.get("command_exit_code"))
    wall = _number(row.get("wall_seconds"))
    timeout = _number(row.get("timeout_seconds"))
    wrapper = row.get("wrapper_status", "")
    if (
        row.get("model") != model
        or exit_code is None
        or wall is None
        or wrapper not in {"COMPLETE", "FAILED", TIMEOUT}
        or (wrapper == "COMPLETE" and int(exit_code) != 0)
        or (wrapper == "FAILED" and int(exit_code) == 0)
        or (wrapper == TIMEOUT and int(exit_code) not in {124, 137})
        or (
            wrapper == TIMEOUT
            and int(exit_code) == 137
            and (timeout is None or float(wall) < float(timeout))
        )
    ):
        return {**empty, "harness_wrapper_status": INVALID,
                "hard_wall_status": INVALID}
    hard_wall = NOT_RUN
    if row.get("run_mode") == "compile":
        if timeout is None or timeout <= 0:
            hard_wall = INVALID
        elif wrapper == TIMEOUT:
            hard_wall = TIMEOUT
        elif float(wall) > float(timeout):
            hard_wall = FAIL
        else:
            hard_wall = PASS
    return {
        "harness_wrapper_status": wrapper,
        "hard_wall_status": hard_wall,
        "hard_wall_timeout_seconds": (
            float(timeout) if timeout is not None else None
        ),
        "harness_wall_seconds": float(wall),
        "harness_exit_code": int(exit_code),
        "harness_failure_stage": row.get("failure_stage", ""),
    }


def _status(
    run_directory: Path, model: str,
) -> tuple[str, int | None, str, float | None, dict[str, Any]]:
    harness = _harness_metrics(run_directory, model)
    if not run_directory.is_dir():
        return NOT_REACHED, None, "not_reached", None, harness
    row = _last_csv_row(run_directory / "status.csv")
    if not row:
        status = (
            TIMEOUT
            if harness["harness_wrapper_status"] == TIMEOUT
            else INCOMPLETE
        )
        stage = harness["harness_failure_stage"] or "incomplete_run"
        return status, harness["harness_exit_code"], stage, harness[
            "harness_wall_seconds"
        ], harness
    raw_status = row.get("status", FAIL) or FAIL
    exit_code = _number(row.get("exit_code"))
    failure_stage = row.get("failure_stage", "")
    total_wall = _number(row.get("total_wall_seconds"))
    if (
        set(row)
        != {"case", "status", "exit_code", "failure_stage", "total_wall_seconds"}
        or row.get("case") != model
        or raw_status not in {PASS, COMPILE_PASS, FAIL}
        or exit_code is None
        or total_wall is None
        or float(total_wall) < 0
        or (
            raw_status in {PASS, COMPILE_PASS}
            and (int(exit_code) != 0 or failure_stage != "")
        )
        or (raw_status == FAIL and int(exit_code) == 0)
    ):
        raw_status = FAIL
        failure_stage = "invalid_status"
    status = raw_status
    if harness["harness_wrapper_status"] == TIMEOUT:
        status = TIMEOUT
        failure_stage = "hard_wall_timeout"
        exit_code = harness["harness_exit_code"]
    elif (
        harness["harness_wrapper_status"] in {"FAILED", INVALID}
        and raw_status in {PASS, COMPILE_PASS}
    ):
        status = FAIL
        failure_stage = harness["harness_failure_stage"] or "campaign_wrapper"
        exit_code = harness["harness_exit_code"]
    return (
        status,
        int(exit_code) if exit_code is not None else None,
        failure_stage,
        float(total_wall) if total_wall is not None else harness[
            "harness_wall_seconds"
        ],
        harness,
    )


def _elf_sizes(run_directory: Path) -> tuple[int | None, int | None, int | None]:
    paths = sorted(run_directory.glob("tile-*.elf"))
    if not paths:
        return None, None, None
    sizes = [path.stat().st_size for path in paths]
    return len(sizes), sum(sizes), max(sizes)


def _active_tiles(run_directory: Path, result: dict[str, str]) -> int | None:
    path = run_directory / "active-cores.txt"
    if path.is_file():
        count = sum(1 for line in path.read_text(encoding="utf-8").splitlines()
                    if line.strip())
        return count
    value = _number(result.get("active_tiles"))
    return int(value) if value is not None else None


def _output_status(run_directory: Path) -> tuple[str, int | None, int | None]:
    path = run_directory / "output-validation.json"
    if not path.is_file():
        return NOT_RUN, None, None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "INVALID", None, None
    outputs = payload.get("outputs")
    if not isinstance(outputs, list):
        return FAIL, None, None
    exact = bool(outputs) and all(
        isinstance(output, dict) and output.get("comparison") == "exact"
        for output in outputs
    )
    status = payload.get("status") == PASS and exact
    return PASS if status else FAIL, len(outputs), sum(
        1 for output in outputs
        if isinstance(output, dict) and output.get("comparison") == "exact"
    )


def _materialization_metrics(run_directory: Path) -> dict[str, Any]:
    path = run_directory / "materialization-audit.json"
    empty = {
        "materialization_audit_status": NOT_RUN,
        "maximum_frame_bytes": None,
        **{field: None for field in MATERIALIZATION_COUNTER_FIELDS},
    }
    if not path.is_file():
        return empty
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {**empty, "materialization_audit_status": "INVALID"}
    if (
        not isinstance(payload, dict)
        or payload.get("schema") != "sculptor.materialization-audit"
        or payload.get("version") != 1
        or payload.get("status") != PASS
        or not isinstance(payload.get("counters"), dict)
    ):
        return {**empty, "materialization_audit_status": FAIL}
    counters = payload["counters"]
    maximum_frame_bytes = payload.get("maximum_frame_bytes")
    if (
        maximum_frame_bytes is not None
        and (
            isinstance(maximum_frame_bytes, bool)
            or not isinstance(maximum_frame_bytes, int)
            or maximum_frame_bytes <= 0
        )
    ):
        return {**empty, "materialization_audit_status": "INVALID"}
    if any(
        field not in counters
        or isinstance(counters[field], bool)
        or not isinstance(counters[field], int)
        for field in MATERIALIZATION_COUNTER_FIELDS
    ):
        return {**empty, "materialization_audit_status": "INVALID"}
    return {
        "materialization_audit_status": PASS,
        "maximum_frame_bytes": maximum_frame_bytes,
        **{field: counters[field] for field in MATERIALIZATION_COUNTER_FIELDS},
    }


def _simulation_metrics(run_directory: Path, result: dict[str, str]) -> dict[str, Any]:
    log_path = run_directory / "simulation.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else ""
    sim_pass = log.count("SCULPTOR_RA_SIM_PASS") if log else None
    fingerprints = log.count("SCULPTOR_MODEL_OUTPUT ") if log else None
    return {
        "simulation_wall_seconds": _number(result.get("wall_seconds")),
        "simulated_time": result.get("simulated_time") or None,
        "synchronization_mode": result.get("synchronization_mode") or None,
        "output_validation": result.get("output_validation") or None,
        "sim_pass_count": sim_pass,
        "output_fingerprints": fingerprints,
    }


def _parse_frequency(value: str) -> float | None:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)(Hz|kHz|MHz|GHz)", value)
    if match is None:
        return None
    scale = {"Hz": 1.0, "kHz": 1e3, "MHz": 1e6, "GHz": 1e9}
    return float(match.group(1)) * scale[match.group(2)]


def _simulated_cycles(
    run_directory: Path, cpu_clock: str, result: dict[str, str] | None = None
) -> int | None:
    """Convert the SST statistics time (ps) to cycles at the CPU clock."""
    frequency = _parse_frequency(cpu_clock)
    if frequency is None:
        return None
    rows = _csv_rows(run_directory / "router-statistics.csv")
    ticks = [
        int(value)
        for row in rows
        for value in [row.get("SimTime", "")]
        if value.isdigit()
    ]
    if not ticks:
        if result is None:
            return None
        match = re.fullmatch(
            r"\s*([0-9]+(?:\.[0-9]+)?)\s*(ps|ns|us|ms|s)\s*",
            result.get("simulated_time", ""),
        )
        if match is None:
            return None
        scale = {"ps": 1e-12, "ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0}
        return int(round(float(match.group(1)) * scale[match.group(2)] * frequency))
    period_ps = 1e12 / frequency
    return int(round(max(ticks) / period_ps))


def _router_metrics(run_directory: Path) -> dict[str, int | None]:
    rows = _csv_rows(run_directory / "router-statistics.csv")
    if not rows:
        return {
            "ram_dma_requests": None,
            "ram_dma_bytes": None,
            "ram_dma_readiness_delay_cycles": None,
            "ram_dma_queue_delay_cycles": None,
            "ram_dma_service_cycles": None,
            "ram_execution_teardown_wait_cycles": None,
            "scratchpad_service_cycles": None,
            "initialization_barrier_wait_cycles": None,
            "initialization_tile_wait_cycles": None,
            "noc_packets": None,
            "noc_flits": None,
            "noc_stall_cycles": None,
            "noc_output_credit_stall_cycles": None,
            "noc_switch_arbitration_stall_cycles": None,
            "noc_input_buffer_full_cycles": None,
        }

    ram: dict[str, int] = {}
    scratchpad_service = 0
    saw_scratchpad_service = False
    initialization: dict[str, int] = {}
    flits = packets = 0
    output_credit = switch = input_full = 0
    saw_flits = saw_packets = False
    saw_output_credit = saw_switch = saw_input_full = False
    fallback_output_port = fallback_xbar = 0
    saw_fallback_output_port = saw_fallback_xbar = False
    for row in rows:
        component = row.get("ComponentName", "")
        port = row.get("StatisticSubId", "")
        name = row.get("StatisticName", "")
        value = _number(row.get("Sum.u64"))
        if value is None:
            continue
        value = int(value)
        if component == "global_ram":
            if name in {
                "requests", "bytes", "readiness_delay_cycles",
                "queue_delay_cycles", "service_cycles",
                "execution_teardown_wait_cycles",
            }:
                ram[name] = ram.get(name, 0) + value
            continue
        if component.startswith("tile") and name == "scratchpad_service_cycles":
            scratchpad_service += value
            saw_scratchpad_service = True
            continue
        if component == "memory_init_barrier" and name in {
            "barrier_wait_cycles", "tile_wait_cycles"
        }:
            initialization[name] = initialization.get(name, 0) + value
            continue
        # Local router egress is a tile endpoint, not a traversed NoC link.
        # Match analyze-performance-profile.py's physical-link accounting.
        if not component.startswith("router_") or port in {"local", "port4"}:
            continue
        if name == "flits_forwarded":
            flits += value
            saw_flits = True
        elif name == "packets_forwarded":
            packets += value
            saw_packets = True
        elif name == "output_credit_stall_cycles":
            output_credit += value
            saw_output_credit = True
        elif name == "output_port_stalls":
            fallback_output_port += value
            saw_fallback_output_port = True
        elif name == "switch_arbitration_stall_cycles":
            switch += value
            saw_switch = True
        elif name == "xbar_stalls":
            fallback_xbar += value
            saw_fallback_xbar = True
        elif name == "input_buffer_full_cycles":
            input_full += value
            saw_input_full = True

    if not saw_output_credit and saw_fallback_output_port:
        output_credit = fallback_output_port
        saw_output_credit = True
    if not saw_switch and saw_fallback_xbar:
        switch = fallback_xbar
        saw_switch = True
    return {
        "ram_dma_requests": ram.get("requests"),
        "ram_dma_bytes": ram.get("bytes"),
        "ram_dma_readiness_delay_cycles": ram.get("readiness_delay_cycles"),
        "ram_dma_queue_delay_cycles": ram.get("queue_delay_cycles"),
        "ram_dma_service_cycles": ram.get("service_cycles"),
        "ram_execution_teardown_wait_cycles": ram.get(
            "execution_teardown_wait_cycles"
        ),
        "scratchpad_service_cycles": (
            scratchpad_service if saw_scratchpad_service else None
        ),
        "initialization_barrier_wait_cycles": initialization.get(
            "barrier_wait_cycles"
        ),
        "initialization_tile_wait_cycles": initialization.get(
            "tile_wait_cycles"
        ),
        "noc_packets": packets if saw_packets else None,
        "noc_flits": flits if saw_flits else None,
        "noc_stall_cycles": (
            output_credit + switch + input_full
            if saw_output_credit or saw_switch or saw_input_full
            else None
        ),
        "noc_output_credit_stall_cycles": output_credit if saw_output_credit else None,
        "noc_switch_arbitration_stall_cycles": switch if saw_switch else None,
        "noc_input_buffer_full_cycles": input_full if saw_input_full else None,
    }


def _stall_metrics(run_directory: Path) -> dict[str, int | str | None]:
    paths = sorted((run_directory / "trace").glob("performance/tile-*-summary.csv"))
    if not paths:
        # Some interrupted runs retain a profile under a diagnostic directory.
        paths = sorted(run_directory.glob("**/performance/tile-*-summary.csv"))
    if not paths:
        return {
            "compute_stall_ticks": None,
            "spm_stall_ticks": None,
            "noc_stall_ticks": None,
            "ram_dma_stall_ticks": None,
            "stall_ticks_total": None,
            "stall_data_source": NA,
        }
    categories = {
        "compute_stall_ticks": 0,
        "spm_stall_ticks": 0,
        "noc_stall_ticks": 0,
        "ram_dma_stall_ticks": 0,
    }
    for path in paths:
        for row in _csv_rows(path):
            name = row.get("metric", "")
            value = _number(row.get("value"))
            if value is None or not name.startswith("wait_") or not name.endswith("_ticks"):
                continue
            value = int(value)
            if name in {"wait_analog-submit_ticks", "wait_analog-wait_ticks"}:
                categories["compute_stall_ticks"] += value
            elif name in {"wait_scratchpad-dma-submit_ticks", "wait_scratchpad-dma-wait_ticks"}:
                categories["ram_dma_stall_ticks"] += value
            elif name in {
                "wait_nic-transmit_ticks",
                "wait_nic-receive-wait_ticks",
                "wait_nic-transmit-wait_ticks",
            }:
                categories["noc_stall_ticks"] += value
            elif name in {
                "wait_memory-access_ticks",
                "wait_memory-batch_ticks",
                "wait_memory-fence_ticks",
            }:
                categories["spm_stall_ticks"] += value
    categories["stall_ticks_total"] = sum(categories.values())
    categories["stall_data_source"] = "trace/performance"
    return categories


def _spm_metrics(run_directory: Path) -> dict[str, int | None]:
    peak = configured = None
    summary_paths = sorted(
        (run_directory / "memory-reports").glob("tile-memory-summary.json")
    )
    if summary_paths:
        try:
            payload = json.loads(summary_paths[0].read_text(encoding="utf-8"))
            capacity = payload.get("summaries", {}).get("capacity", {})
            maximums = capacity.get("maximums", {})
            peak = maximums.get("requiredLocalBytes")
            configured = maximums.get("configuredSPMBytes")
        except (OSError, json.JSONDecodeError, AttributeError):
            pass
    deployment = run_directory / "deployment" / "09-tile-deployment.mlir"
    if deployment.is_file():
        text = deployment.read_text(encoding="utf-8", errors="replace")
        required = [int(value) for value in re.findall(r"requiredLocalBytes\s*=\s*([0-9]+)", text)]
        configured_values = [
            int(value)
            for value in re.findall(
                r"configuredSPMBytes\s*=\s*([0-9]+)", text
            )
        ]
        if peak is None:
            peak = max(required) if required else None
        if configured is None:
            configured = max(configured_values) if configured_values else None
    return {"spm_peak_bytes": peak, "spm_configured_bytes": configured}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _qualification_empty(status: str = NOT_RUN) -> dict[str, Any]:
    return {
        "compile_qualification_status": status,
        "feature_configuration_sha256": None,
        "lowering_status": NOT_RUN,
        "abi_preflight_status": NOT_RUN,
        "abi_preflight_passes": None,
        "elf_generation_status": NOT_RUN,
        "qualified_elf_count": None,
        "optimization_contract_status": NOT_RUN,
        "execution_residency_audit_status": NOT_RUN,
        "execution_residency_mode": None,
        "execution_residency_phase": None,
        "execution_residency_physical_change_count": None,
        "execution_residency_audit_sha256": None,
        "execution_residency_maximum_members": None,
        "execution_residency_maximum_wave_width": None,
        "residency_contract_status": NOT_RUN,
        "residency_activated": None,
        "residency_selected_count": None,
        "direct_forwarding_contract_status": NOT_RUN,
        "direct_forwarding_activated": None,
        "direct_forwarding_selected_count": None,
        "persistent_matrices_contract_status": NOT_RUN,
        "persistent_matrices_activated": None,
        "persistent_matrix_count": None,
        "buffering_contract_status": NOT_RUN,
        "buffering_activated": None,
        "double_buffering_activated": None,
        "single_slot_record_count": None,
        "double_slot_record_count": None,
        "digital_workers_contract_status": NOT_RUN,
        "digital_workers_activated": None,
        "configured_digital_workers": None,
        "configured_max_in_flight": None,
        "expanded_digital_operation_count": None,
        "expanded_digital_work_unit_count": None,
        "exact_dependencies_contract_status": NOT_RUN,
        "exact_dependencies_activated": None,
        "synchronization_contract_mode": None,
        "semantic_epoch_count": None,
        "local_epoch_task_count": None,
        "runtime_local_epoch_progress_observed": None,
        "configurable_frames_contract_status": NOT_RUN,
        "configured_maximum_frame_bytes": None,
        "supported_maximum_frame_bytes": None,
        "observed_maximum_frame_bytes": None,
        "feature_activation_count": None,
    }


def _compile_qualification(
    run_directory: Path, model: str
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    path = run_directory / "compile-qualification.json"
    if not path.is_file():
        return _qualification_empty(), None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return _qualification_empty(INVALID), None
    if not isinstance(payload, dict):
        return _qualification_empty(INVALID), None
    if payload.get("status") != PASS:
        return _qualification_empty(FAIL), payload
    checks = payload.get("checks")
    features = payload.get("features")
    configuration = payload.get("configuration")
    if (
        payload.get("schema") != "sculptor.compile-qualification"
        or payload.get("version") != 1
        or payload.get("model") != model
        or payload.get("errors") != []
        or payload.get("output_value_validation_required") is not False
        or not isinstance(checks, dict)
        or not isinstance(features, dict)
        or set(features) != set(FEATURE_NAMES)
        or not isinstance(configuration, dict)
    ):
        return _qualification_empty(INVALID), payload
    required_checks = {
        "complete_lowering",
        "execution_residency_audit",
        "materialization_audit",
        "abi_preflight",
        "elf_generation",
        "memory_validation",
        "optimization_contracts",
    }
    if set(checks) != required_checks or any(
        not isinstance(checks[name], dict) or checks[name].get("status") != PASS
        for name in required_checks
    ):
        return _qualification_empty(INVALID), payload
    if any(
        not isinstance(features[name], dict)
        or features[name].get("contract_status") != PASS
        or not isinstance(features[name].get("activated"), bool)
        for name in FEATURE_NAMES
    ):
        return _qualification_empty(INVALID), payload
    encoded = json.dumps(
        configuration, sort_keys=True, separators=(",", ":")
    ).encode()
    fingerprint = hashlib.sha256(encoded).hexdigest()
    if payload.get("configuration_sha256") != fingerprint:
        return _qualification_empty(INVALID), payload

    residency = features["residency"]
    direct = features["direct_forwarding"]
    persistent = features["persistent_matrices"]
    buffering = features["circular_double_buffering"]
    digital = features["digital_workers"]
    exact = features["exact_dependencies_local_epoch"]
    frames = features["configurable_frames"]
    execution_residency = checks["execution_residency_audit"]
    activated = sum(bool(features[name]["activated"]) for name in FEATURE_NAMES)
    return {
        "compile_qualification_status": PASS,
        "feature_configuration_sha256": fingerprint,
        "lowering_status": checks["complete_lowering"]["status"],
        "abi_preflight_status": checks["abi_preflight"]["status"],
        "abi_preflight_passes": checks["abi_preflight"].get("passes"),
        "elf_generation_status": checks["elf_generation"]["status"],
        "qualified_elf_count": checks["elf_generation"].get("tile_elf_count"),
        "optimization_contract_status": checks["optimization_contracts"]["status"],
        "execution_residency_audit_status": execution_residency["status"],
        "execution_residency_mode": execution_residency.get("mode"),
        "execution_residency_phase": execution_residency.get("phase"),
        "execution_residency_physical_change_count": execution_residency.get(
            "physical_change_count"
        ),
        "execution_residency_audit_sha256": execution_residency.get(
            "audit_sha256"
        ),
        "execution_residency_maximum_members": configuration.get(
            "execution_residency_maximum_members"
        ),
        "execution_residency_maximum_wave_width": configuration.get(
            "execution_residency_maximum_wave_width"
        ),
        "residency_contract_status": residency["contract_status"],
        "residency_activated": residency["activated"],
        "residency_selected_count": residency.get("selected_component_count"),
        "direct_forwarding_contract_status": direct["contract_status"],
        "direct_forwarding_activated": direct["activated"],
        "direct_forwarding_selected_count": direct.get("selected_route_count"),
        "persistent_matrices_contract_status": persistent["contract_status"],
        "persistent_matrices_activated": persistent["activated"],
        "persistent_matrix_count": persistent.get("persistent_matrix_count"),
        "buffering_contract_status": buffering["contract_status"],
        "buffering_activated": buffering["activated"],
        "double_buffering_activated": buffering.get("double_buffering_activated"),
        "single_slot_record_count": buffering.get("single_slot_record_count"),
        "double_slot_record_count": buffering.get("double_slot_record_count"),
        "digital_workers_contract_status": digital["contract_status"],
        "digital_workers_activated": digital["activated"],
        "configured_digital_workers": digital.get("configured_worker_limit"),
        "configured_max_in_flight": configuration.get("max_in_flight"),
        "expanded_digital_operation_count": digital.get("expanded_operation_count"),
        "expanded_digital_work_unit_count": digital.get("expanded_work_unit_count"),
        "exact_dependencies_contract_status": exact["contract_status"],
        "exact_dependencies_activated": exact["activated"],
        "synchronization_contract_mode": exact.get("synchronization_mode"),
        "semantic_epoch_count": exact.get("semantic_epoch_count"),
        "local_epoch_task_count": exact.get("local_epoch_task_count"),
        "runtime_local_epoch_progress_observed": exact.get(
            "runtime_progress_observed"
        ),
        "configurable_frames_contract_status": frames["contract_status"],
        "configured_maximum_frame_bytes": frames.get(
            "configured_maximum_frame_bytes"
        ),
        "supported_maximum_frame_bytes": ";".join(
            str(value) for value in frames.get("supported_maximum_frame_bytes", [])
        ),
        "observed_maximum_frame_bytes": ";".join(
            str(value) for value in frames.get("observed_maximum_frame_bytes", [])
        ),
        "feature_activation_count": activated,
    }, payload


def _run_manifest_contract(
    run_directory: Path,
    model: str,
    qualification: dict[str, Any] | None,
) -> tuple[str, str | None]:
    path = run_directory / "run-manifest.json"
    if not path.is_file():
        return NOT_RUN, None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return INVALID, None
    if not isinstance(payload, dict):
        return INVALID, None
    run = payload.get("run")
    artifacts = payload.get("artifacts")
    environment = payload.get("environment")
    if (
        payload.get("schema") != "golem.sculptor-run"
        or payload.get("schema_version") != 1
        or not isinstance(run, dict)
        or run.get("model") != model
        or run.get("mode") not in {"compile", "simulation"}
        or not isinstance(artifacts, dict)
        or not isinstance(environment, dict)
        or qualification is None
    ):
        return INVALID, run.get("mode") if isinstance(run, dict) else None
    required_artifacts = {
        "model_mlir",
        "expected_outputs",
        "deployment",
        "core_objects",
        "active_cores",
        "deployment_manifest",
        "abi_preflight_summary",
        "abi_preflight_logs",
        "materialization_audit",
        "execution_residency_audit",
        "memory_reports",
        "compile_qualification",
        "idle_elf",
    }
    if not required_artifacts.issubset(artifacts):
        return FAIL, run["mode"]
    record = artifacts.get("compile_qualification")
    certificate = run_directory / "compile-qualification.json"
    if (
        not isinstance(record, dict)
        or record.get("kind") != "file"
        or not certificate.is_file()
        or record.get("sha256") != _sha256(certificate)
    ):
        return FAIL, run["mode"]
    try:
        if Path(record["resolved_path"]).resolve(strict=True) != certificate.resolve(
            strict=True
        ):
            return FAIL, run["mode"]
    except (OSError, KeyError, TypeError):
        return FAIL, run["mode"]
    active = qualification.get("active_tile_ids")
    if not isinstance(active, list):
        return FAIL, run["mode"]
    required_tiles = {f"tile_{tile}_elf" for tile in active}
    actual_tiles = {name for name in artifacts if re.fullmatch(r"tile_[0-9]+_elf", name)}
    if actual_tiles != required_tiles:
        return FAIL, run["mode"]

    config = qualification.get("configuration")
    if not isinstance(config, dict):
        return FAIL, run["mode"]
    expected_environment = {
        "GOLEM_MODEL_FIXED_SHARD_BYTES": str(config.get("fixed_shard_bytes")),
        "GOLEM_MODEL_MAX_IN_FLIGHT": str(config.get("max_in_flight")),
        "GOLEM_MODEL_DIGITAL_WORKERS": str(config.get("digital_workers")),
        "GOLEM_MODEL_RETAIN_PROVED_LOCAL_OWNERS": "1",
    }
    if any(environment.get(name) != value for name, value in expected_environment.items()):
        return FAIL, run["mode"]
    environment_execution_mode = {
        "0": "off", "off": "off", "analyze": "analyze",
        "1": "select", "select": "select",
    }.get(environment.get("GOLEM_MODEL_EXECUTION_RESIDENCY_REGIONS"))
    if environment_execution_mode != config.get("execution_residency_mode"):
        return FAIL, run["mode"]
    for environment_name, configuration_name in (
        ("GOLEM_MODEL_EXECUTION_RESIDENCY_MAXIMUM_MEMBERS",
         "execution_residency_maximum_members"),
        ("GOLEM_MODEL_EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH",
         "execution_residency_maximum_wave_width"),
    ):
        if environment.get(environment_name) != str(config.get(configuration_name)):
            return FAIL, run["mode"]
    expected_positive_benefit = config.get(
        "execution_residency_require_positive_benefit"
    )
    environment_positive_benefit = environment.get(
        "GOLEM_MODEL_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT"
    )
    if (
        not isinstance(expected_positive_benefit, bool)
        or environment_positive_benefit not in {"0", "1", "false", "true"}
        or (environment_positive_benefit in {"1", "true"})
        != expected_positive_benefit
    ):
        return FAIL, run["mode"]
    if environment.get("GOLEM_MODEL_RETAINED_OWNER_BOUNDARY_IDS"):
        return FAIL, run["mode"]
    return PASS, run["mode"]


def collect_row(run_directory: Path, model: str, cpu_clock: str) -> dict[str, Any]:
    status, exit_code, failure_stage, total_wall, harness = _status(
        run_directory, model
    )
    result = _last_csv_row(run_directory / "result.csv")
    resource_wall, max_rss = _resource_usage(run_directory / "resource-usage.csv")
    elf_count, elf_bytes, elf_bytes_max = _elf_sizes(run_directory)
    exact_status, output_count, exact_count = _output_status(run_directory)
    simulation = _simulation_metrics(run_directory, result)
    router = _router_metrics(run_directory)
    stalls = _stall_metrics(run_directory)
    spm = _spm_metrics(run_directory)
    materialization = _materialization_metrics(run_directory)
    qualification_metrics, qualification = _compile_qualification(
        run_directory, model
    )
    run_manifest_status, manifest_run_mode = _run_manifest_contract(
        run_directory, model, qualification
    )
    active_tiles = _active_tiles(run_directory, result)
    sim_pass = simulation["sim_pass_count"]
    sim_pass_status = (
        PASS if sim_pass is not None and active_tiles is not None and sim_pass == active_tiles
        else (FAIL if sim_pass is not None else NOT_RUN)
    )
    return {
        "model": model,
        "order": REQUIRED_MODELS.index(model) + 1,
        "status": status,
        "manifest_run_mode": manifest_run_mode,
        "run_manifest_contract_status": run_manifest_status,
        "exit_code": exit_code,
        "failure_stage": failure_stage,
        "total_wall_seconds": total_wall if total_wall is not None else resource_wall,
        "max_rss_kb": max_rss,
        "compiler_wall_seconds": _sum_stage_timings(run_directory / "stage-timings.csv"),
        **harness,
        "elf_count": elf_count,
        "elf_bytes_total": elf_bytes,
        "elf_bytes_max": elf_bytes_max,
        "simulation_wall_seconds": simulation["simulation_wall_seconds"],
        "simulated_time": simulation["simulated_time"],
        "simulated_cycles": _simulated_cycles(run_directory, cpu_clock, result),
        "synchronization_mode": simulation["synchronization_mode"],
        "output_validation": simulation["output_validation"],
        "active_tiles": active_tiles,
        "sim_pass_count": sim_pass,
        "sim_pass_status": sim_pass_status,
        "spm_peak_bytes": spm["spm_peak_bytes"],
        "exact_peak_tile_bytes": spm["spm_peak_bytes"],
        "spm_configured_bytes": spm["spm_configured_bytes"],
        **materialization,
        **router,
        **stalls,
        "output_fingerprints": simulation["output_fingerprints"],
        "output_count": output_count,
        "exact_output_count": exact_count,
        "exact_output_status": exact_status,
        # Keep the old column name as a compatibility alias for consumers of
        # full-model-summary.csv.
        "correctness": exact_status,
        **qualification_metrics,
        "run_directory": str(run_directory),
    }


FIELDS = [
    "model", "order", "status", "manifest_run_mode",
    "run_manifest_contract_status", "exit_code", "failure_stage",
    "total_wall_seconds", "max_rss_kb", "compiler_wall_seconds",
    "harness_wrapper_status", "hard_wall_status",
    "hard_wall_timeout_seconds", "harness_wall_seconds",
    "harness_exit_code", "harness_failure_stage",
    "elf_count", "elf_bytes_total", "elf_bytes_max",
    "simulation_wall_seconds", "simulated_time", "simulated_cycles",
    "synchronization_mode", "output_validation",
    "active_tiles", "sim_pass_count", "sim_pass_status",
    "spm_peak_bytes", "exact_peak_tile_bytes", "spm_configured_bytes",
    "materialization_audit_status", "maximum_frame_bytes",
    *MATERIALIZATION_COUNTER_FIELDS,
    "ram_dma_requests", "ram_dma_bytes", "ram_dma_readiness_delay_cycles",
    "ram_dma_queue_delay_cycles", "ram_dma_service_cycles",
    "ram_execution_teardown_wait_cycles", "scratchpad_service_cycles",
    "initialization_barrier_wait_cycles", "initialization_tile_wait_cycles",
    "noc_packets", "noc_flits",
    "noc_stall_cycles", "noc_output_credit_stall_cycles",
    "noc_switch_arbitration_stall_cycles", "noc_input_buffer_full_cycles",
    "compute_stall_ticks", "spm_stall_ticks", "noc_stall_ticks",
    "ram_dma_stall_ticks", "stall_ticks_total", "stall_data_source",
    "compile_qualification_status", "feature_configuration_sha256",
    "lowering_status", "abi_preflight_status", "abi_preflight_passes",
    "elf_generation_status", "qualified_elf_count",
    "optimization_contract_status",
    "execution_residency_audit_status", "execution_residency_mode",
    "execution_residency_phase", "execution_residency_physical_change_count",
    "execution_residency_audit_sha256",
    "execution_residency_maximum_members",
    "execution_residency_maximum_wave_width",
    "residency_contract_status", "residency_activated",
    "residency_selected_count",
    "direct_forwarding_contract_status", "direct_forwarding_activated",
    "direct_forwarding_selected_count",
    "persistent_matrices_contract_status", "persistent_matrices_activated",
    "persistent_matrix_count", "buffering_contract_status",
    "buffering_activated", "double_buffering_activated",
    "single_slot_record_count", "double_slot_record_count",
    "digital_workers_contract_status", "digital_workers_activated",
    "configured_digital_workers", "configured_max_in_flight",
    "expanded_digital_operation_count",
    "expanded_digital_work_unit_count", "exact_dependencies_contract_status",
    "exact_dependencies_activated", "synchronization_contract_mode",
    "semantic_epoch_count", "local_epoch_task_count",
    "runtime_local_epoch_progress_observed",
    "configurable_frames_contract_status", "configured_maximum_frame_bytes",
    "supported_maximum_frame_bytes", "observed_maximum_frame_bytes",
    "feature_activation_count",
    "output_fingerprints", "output_count", "exact_output_count",
    "exact_output_status", "correctness", "run_directory",
]


def _json_value(value: Any) -> Any:
    return None if value == NA else value


def write_report(
    rows: list[dict[str, Any]],
    csv_path: Path,
    json_path: Path,
    strict: bool,
    completion_mode: str = "simulation",
    require_exact_outputs: bool = False,
) -> bool:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({
                field: NA if row.get(field) is None else row.get(field)
                for field in FIELDS
            })

    expected_status = COMPILE_PASS if completion_mode == "compile" else PASS
    not_reached = [
        row["model"] for row in rows if row["status"] == NOT_REACHED
    ]
    incomplete = [
        row["model"] for row in rows if row["status"] == INCOMPLETE
    ]
    timed_out = [row["model"] for row in rows if row["status"] == TIMEOUT]
    unaudited = [
        row["model"]
        for row in rows
        if row["materialization_audit_status"] != PASS
    ]
    unqualified = [
        row["model"]
        for row in rows
        if row["compile_qualification_status"] != PASS
    ]
    unmanifested = [
        row["model"]
        for row in rows
        if row["run_manifest_contract_status"] != PASS
    ]
    unbounded = [
        row["model"] for row in rows if row["hard_wall_status"] != PASS
    ]
    configuration_violations = [
        row["model"]
        for row in rows
        if row["compile_qualification_status"] == PASS
        and (
            not isinstance(row["configured_digital_workers"], int)
            or row["configured_digital_workers"] < 2
            or row["configured_max_in_flight"] != 2
        )
    ]
    fingerprints = {
        row["feature_configuration_sha256"]
        for row in rows
        if row["feature_configuration_sha256"] is not None
    }
    uniform_configuration = (
        len(fingerprints) == 1
        and all(row["feature_configuration_sha256"] is not None for row in rows)
    )
    nonuniform_configuration = (
        [row["model"] for row in rows] if len(fingerprints) > 1 else []
    )

    compile_qualified_models: list[str] = []
    if completion_mode == "compile":
        for row in rows:
            if (
                row["status"] == expected_status
                and row["materialization_audit_status"] == PASS
                and row["compile_qualification_status"] == PASS
                and row["run_manifest_contract_status"] == PASS
                and row["hard_wall_status"] == PASS
                and row["model"] not in configuration_violations
                and row["model"] not in nonuniform_configuration
            ):
                compile_qualified_models.append(row["model"])
        failed = [
            row["model"]
            for row in rows
            if row["model"] not in compile_qualified_models
        ]
    else:
        failed = [
            row["model"] for row in rows if row["status"] != expected_status
        ]
    non_exact = []
    incomplete_tiles = []
    if completion_mode == "simulation":
        non_exact = [
            row["model"]
            for row in rows
            if row["exact_output_status"] != PASS
        ]
        incomplete_tiles = [
            row["model"]
            for row in rows
            if row["status"] == PASS and row["sim_pass_status"] != PASS
        ]
    else:
        incomplete_tiles = [
            row["model"]
            for row in rows
            if row["abi_preflight_status"] != PASS
        ]
    feature_columns = {
        "residency": ("residency_contract_status", "residency_activated"),
        "direct_forwarding": (
            "direct_forwarding_contract_status",
            "direct_forwarding_activated",
        ),
        "persistent_matrices": (
            "persistent_matrices_contract_status",
            "persistent_matrices_activated",
        ),
        "circular_double_buffering": (
            "buffering_contract_status",
            "double_buffering_activated",
        ),
        "digital_workers": (
            "digital_workers_contract_status",
            "digital_workers_activated",
        ),
        "exact_dependencies_local_epoch": (
            "exact_dependencies_contract_status",
            "exact_dependencies_activated",
        ),
        "configurable_frames": (
            "configurable_frames_contract_status",
            None,
        ),
    }
    feature_coverage: dict[str, Any] = {}
    for feature, (contract_column, activation_column) in feature_columns.items():
        contract_pass = [
            row["model"] for row in rows if row[contract_column] == PASS
        ]
        activated = (
            [row["model"] for row in rows if row[activation_column] is True]
            if activation_column is not None
            else [
                row["model"]
                for row in rows
                if row["configured_maximum_frame_bytes"] is not None
            ]
        )
        feature_coverage[feature] = {
            "contract_pass_models": contract_pass,
            "activated_models": activated,
            "inactive_models": [
                row["model"]
                for row in rows
                if row[contract_column] == PASS and row["model"] not in activated
            ],
            "not_proven_models": [
                row["model"] for row in rows if row[contract_column] != PASS
            ],
        }
    observed_frames = sorted(
        {
            int(value)
            for row in rows
            if row["observed_maximum_frame_bytes"]
            for value in str(row["observed_maximum_frame_bytes"]).split(";")
            if value
        }
    )
    all_feature_contracts_pass = all(
        len(feature["contract_pass_models"]) == len(REQUIRED_MODELS)
        for feature in feature_coverage.values()
    )
    never_activated_features = [
        name
        for name, feature in feature_coverage.items()
        if not feature["activated_models"]
    ]
    if completion_mode == "compile":
        complete = (
            len(compile_qualified_models) == len(REQUIRED_MODELS)
            and uniform_configuration
            and all_feature_contracts_pass
        )
    else:
        complete = not (
            not_reached
            or failed
            or unaudited
            or incomplete_tiles
            or (require_exact_outputs and non_exact)
        )

    validation = {
        "completion_mode": completion_mode,
        "exact_outputs_required": require_exact_outputs,
        "required_model_count": len(REQUIRED_MODELS),
        "qualified_model_count": len(compile_qualified_models),
        "qualification_ratio": (
            f"{len(compile_qualified_models)}/{len(REQUIRED_MODELS)}"
        ),
        "all_required_models_present": not not_reached,
        "all_models_compile": (
            len(compile_qualified_models) == len(REQUIRED_MODELS)
            if completion_mode == "compile"
            else not not_reached and not failed
        ),
        "all_models_pass": not failed,
        "all_materialization_audits_pass": not unaudited,
        "all_exact_outputs_pass": not non_exact,
        "all_active_tiles_pass": not incomplete_tiles,
        "all_feature_contracts_pass": all_feature_contracts_pass,
        "activation_coverage_complete": not never_activated_features,
        "never_activated_features": never_activated_features,
        "uniform_feature_configuration": uniform_configuration,
        "complete": complete,
        "missing_models": not_reached,
        "not_reached_models": not_reached,
        "incomplete_models": incomplete,
        "timed_out_models": timed_out,
        "failed_models": failed,
        "compile_qualified_models": compile_qualified_models,
        "unqualified_models": unqualified,
        "run_manifest_contract_failed_models": unmanifested,
        "unbounded_models": unbounded,
        "configuration_violation_models": configuration_violations,
        "nonuniform_configuration_models": nonuniform_configuration,
        "unaudited_models": unaudited,
        "non_exact_models": non_exact,
        "incomplete_tile_models": incomplete_tiles,
        "observed_maximum_frame_bytes": observed_frames,
        "feature_coverage": feature_coverage,
    }
    payload = {
        "schema_version": 1,
        "required_models": list(REQUIRED_MODELS),
        "models": [{key: _json_value(value) for key, value in row.items()} for row in rows],
        "validation": validation,
    }
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if strict and not validation["complete"]:
        print(
            "model-suite report validation failed: "
            + json.dumps(validation, sort_keys=True),
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-root", type=Path, required=True)
    parser.add_argument("--run-tag", required=True)
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--cpu-clock", default="1GHz")
    parser.add_argument(
        "--completion-mode",
        choices=("simulation", "compile"),
        default="simulation",
        help="report semantics even for a non-strict/interrupted campaign",
    )
    completion = parser.add_mutually_exclusive_group()
    completion.add_argument("--require-complete", action="store_true")
    completion.add_argument("--require-compile-complete", action="store_true")
    parser.add_argument(
        "--require-exact-outputs",
        action="store_true",
        help=(
            "make numerical output comparison a simulation completion gate; "
            "the V1 residency campaign leaves this disabled"
        ),
    )
    args = parser.parse_args()

    rows = [
        collect_row(args.results_root / model / args.run_tag, model, args.cpu_clock)
        for model in REQUIRED_MODELS
    ]
    mode = "compile" if args.require_compile_complete else args.completion_mode
    if args.require_complete and mode != "simulation":
        parser.error("--require-complete requires --completion-mode simulation")
    strict = args.require_complete or args.require_compile_complete
    return 0 if write_report(
        rows,
        args.output_csv,
        args.output_json,
        strict,
        completion_mode=mode,
        require_exact_outputs=args.require_exact_outputs,
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
