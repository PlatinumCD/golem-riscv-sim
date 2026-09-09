#!/usr/bin/env python3
"""Fail-closed qualification of one fully compiled Sculptor model.

This validator consumes only compiler-side artifacts.  It does not execute a
compiler, QEMU, or SST.  A PASS certificate proves that lowering reached every
required stage, every active tile was materialized and ABI-preflighted, every
production ELF exists, and the model-independent optimization contracts are
present in every finalized tile.  Runtime execution and numerical output
values are deliberately outside this certificate.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import struct
import sys
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tests" / "support"))
from deployment_manifest import load_deployment_manifest  # noqa: E402


PASS = "PASS"
SCALAR_PLAN_VERSION = 3
PHYSICAL_EXECUTION_RESIDENCY_CERTIFICATE_VERSION = 2
SCALAR_INCREMENTAL_FIELDS = (
    "incremental_over_retained_control_input_descriptor_count",
    "incremental_over_retained_control_input_request_count",
    "incremental_over_retained_control_input_bytes",
    "incremental_over_retained_control_output_descriptor_count",
    "incremental_over_retained_control_output_request_count",
    "incremental_over_retained_control_output_bytes",
)
SUPPORTED_FRAME_BYTES = (4096, 8192, 16384, 32768, 65536, 131072, 262144)
REQUIRED_LOWERING_STAGES = (
    "01-canonical",
    "02-converted",
    "03-layouts",
    "03-golem",
    "03-resolved-layouts",
    "04-expanded-digital-work",
    "04-parametric-work",
    "04-tensor-fragments",
    "04-residency-regions",
    "05-ra-tree",
    "06-mapping-plan",
    "08-placed",
    "09-tile-deployment-split",
)
REQUIRED_LOWERING_FILES = (
    "01-canonical.mlir",
    "02-converted.mlir",
    "03-layouts.mlir",
    "03-golem.mlir",
    "03-resolved-layouts.mlir",
    "04-expanded-digital-work.mlir",
    "04-parametric-work.mlir",
    "04-tensor-fragments.mlir",
    "04-residency-regions.mlir",
    "05-ra-tree.mlir",
    "06-mapping-plan.mlir",
    "08-placed.mlir",
)
REQUIRED_CORE_SUFFIXES = (
    "-extracted.mlir",
    "-runtime-graph.mlir",
    "-planned.mlir",
    "-finalized.mlir",
    "-compute-llvm.mlir",
    "-task-only.mlir",
    ".ll",
    ".o",
)
CORRECTNESS_COUNTERS = (
    "unowned_materialized_byte_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "cross_epoch_direct_route_count",
    "unclassified_boundary_count",
)
PERSISTENT_FIELDS = (
    "persistent_matrix_count",
    "persistent_matrix_programmed_bytes",
    "persistent_matrix_consumer_task_count",
    "persistent_matrix_consumer_execution_count",
    "persistent_matrix_maximum_last_use_epoch",
    "persistent_matrix_duplicate_setup_count",
    "persistent_matrix_unproven_use_count",
    "persistent_matrix_repeated_setup_count",
)
FEATURE_NAMES = (
    "residency",
    "direct_forwarding",
    "persistent_matrices",
    "circular_double_buffering",
    "digital_workers",
    "exact_dependencies_local_epoch",
    "configurable_frames",
)


class QualificationError(RuntimeError):
    """A missing, malformed, or contradictory qualification artifact."""

    def __init__(self, check: str, message: str):
        super().__init__(message)
        self.check = check


def fail(check: str, message: str) -> None:
    raise QualificationError(check, message)


def read_json(path: Path, check: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(check, f"cannot read {path}: {error}")
    if not isinstance(value, dict):
        fail(check, f"{path} must contain a JSON object")
    return value


def require_nonempty_file(path: Path, check: str) -> None:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        fail(check, f"missing, empty, or symbolic-link artifact: {path}")


def read_active_tiles(path: Path, network_size: int) -> list[int]:
    require_nonempty_file(path, "complete_lowering")
    lines = path.read_text(encoding="utf-8").splitlines()
    if any(re.fullmatch(r"[0-9]+", line) is None for line in lines):
        fail("complete_lowering", "active-core manifest is not canonical")
    tiles = [int(line) for line in lines]
    if tiles != sorted(set(tiles)):
        fail("complete_lowering", "active-core IDs are not unique canonical order")
    if any(tile >= network_size for tile in tiles):
        fail("complete_lowering", "active-core ID is outside the configured mesh")
    return tiles


def validate_architecture(
    run: Path, expected_frame: int, expected_max_in_flight: int
) -> dict[str, int]:
    check = "configurable_frames"
    payload = read_json(run / "streaming-architecture.json", check)
    if set(payload) != {"schema", "schema_version", "source_mlir", "architecture"}:
        fail(check, "streaming architecture fields are invalid")
    if payload["schema"] != "golem.streaming-architecture" or payload["schema_version"] != 1:
        fail(check, "streaming architecture schema is invalid")
    architecture = payload.get("architecture")
    required = {
        "version",
        "fixed_shard_bytes",
        "scratchpad_bytes",
        "global_ram_bytes",
        "max_in_flight",
        "noc_word_bytes",
    }
    if not isinstance(architecture, dict) or set(architecture) != required:
        fail(check, "streaming architecture contract is incomplete")
    if any(isinstance(architecture[name], bool) or not isinstance(architecture[name], int)
           for name in required):
        fail(check, "streaming architecture values must be integers")
    if architecture["version"] != 1 or architecture["noc_word_bytes"] != 4:
        fail(check, "streaming architecture version or NoC word is invalid")
    if architecture["fixed_shard_bytes"] != expected_frame:
        fail(check, "configured maximum frame disagrees with the harness")
    if architecture["fixed_shard_bytes"] not in SUPPORTED_FRAME_BYTES:
        fail(check, "configured maximum frame is outside 4--256 KiB")
    if architecture["max_in_flight"] != expected_max_in_flight:
        fail(check, "max-in-flight disagrees with the harness")
    if architecture["scratchpad_bytes"] < expected_frame:
        fail(check, "scratchpad is smaller than the configured maximum frame")
    try:
        source = Path(payload["source_mlir"]).resolve(strict=True)
        placed = (run / "deployment" / "08-placed.mlir").resolve(strict=True)
    except (OSError, TypeError) as error:
        fail(check, f"invalid architecture source: {error}")
    if source != placed:
        fail(check, "architecture source is not deployment/08-placed.mlir")
    return architecture


def validate_lowering(run: Path, active_tiles: list[int]) -> dict[str, Any]:
    check = "complete_lowering"
    deployment = run / "deployment"
    metrics_path = deployment / "compiler-stage-metrics.csv"
    require_nonempty_file(metrics_path, check)
    with metrics_path.open("r", encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source))
    by_stage: dict[str, dict[str, str]] = {}
    for row in rows:
        stage = row.get("stage", "")
        if not stage or stage in by_stage:
            fail(check, f"compiler stage metrics repeat or omit a stage name: {stage!r}")
        by_stage[stage] = row
    for stage in REQUIRED_LOWERING_STAGES:
        row = by_stage.get(stage)
        if row is None:
            fail(check, f"compiler stage was not reached: {stage}")
        if row.get("exit_code") != "0":
            fail(check, f"compiler stage did not pass: {stage}")
        try:
            if int(row.get("output_bytes", "0")) <= 0:
                fail(check, f"compiler stage emitted no output: {stage}")
        except ValueError:
            fail(check, f"compiler stage output accounting is malformed: {stage}")
    for relative in REQUIRED_LOWERING_FILES:
        require_nonempty_file(deployment / relative, check)

    core_directory = run / "cores"
    if core_directory.is_symlink() or not core_directory.is_dir():
        fail(check, "core artifact directory is missing or symbolic")
    for suffix in REQUIRED_CORE_SUFFIXES:
        actual: set[int] = set()
        pattern = re.compile(
            r"core-([0-9]+)" + re.escape(suffix) + r"\Z"
        )
        for path in core_directory.iterdir():
            match = pattern.fullmatch(path.name)
            if match is None:
                continue
            require_nonempty_file(path, check)
            actual.add(int(match.group(1)))
        if actual != set(active_tiles):
            fail(
                check,
                f"core {suffix} artifacts disagree with active tiles: "
                f"missing={sorted(set(active_tiles) - actual)}, "
                f"unexpected={sorted(actual - set(active_tiles))}",
            )
    return {
        "status": PASS,
        "required_stage_count": len(REQUIRED_LOWERING_STAGES),
        "completed_stage_count": len(REQUIRED_LOWERING_STAGES),
        "active_tile_count": len(active_tiles),
        "core_artifact_kinds": len(REQUIRED_CORE_SUFFIXES),
    }


def require_int(mapping: dict[str, Any], name: str, check: str) -> int:
    value = mapping.get(name)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        fail(check, f"{name} must be a nonnegative integer")
    return value


def validate_materialization(
    run: Path,
    active_tiles: list[int],
    architecture: dict[str, int],
    epoch_count: int,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    check = "materialization_audit"
    payload = read_json(run / "materialization-audit.json", check)
    if (
        payload.get("schema") != "sculptor.materialization-audit"
        or payload.get("version") != 1
        or payload.get("status") != PASS
        or payload.get("errors") != []
    ):
        fail(check, "materialization audit is not a clean PASS certificate")
    if payload.get("active_tile_ids") != active_tiles:
        fail(check, "materialization audit active tiles are not exhaustive")
    if payload.get("maximum_frame_bytes") != architecture["fixed_shard_bytes"]:
        fail(check, "materialization audit maximum frame disagrees")
    if payload.get("global_ram_bytes") != architecture["global_ram_bytes"]:
        fail(check, "materialization audit global RAM disagrees")
    counters = payload.get("counters")
    if not isinstance(counters, dict):
        fail(check, "materialization counters are missing")
    if require_int(counters, "epoch_count", check) != epoch_count:
        fail(check, "materialization and deployment epoch counts disagree")
    nonzero = {
        name: require_int(counters, name, check)
        for name in CORRECTNESS_COUNTERS
        if require_int(counters, name, check) != 0
    }
    if nonzero:
        fail(check, f"materialization correctness counters are nonzero: {nonzero}")

    tile_count = len(active_tiles)
    phase4_tiles = require_int(counters, "phase4_accounting_tile_count", check)
    phase5_tiles = require_int(counters, "phase5_accounting_tile_count", check)
    policy_tiles = require_int(
        counters, "retained_local_policy_enabled_tile_count", check
    )
    if (phase4_tiles, phase5_tiles, policy_tiles) != (tile_count, tile_count, tile_count):
        fail(check, "residency/direct-forward accounting does not cover every tile")
    tiles = payload.get("tiles")
    if not isinstance(tiles, list) or len(tiles) != tile_count:
        fail(check, "materialization per-tile records are incomplete")
    if [tile.get("tile_id") for tile in tiles if isinstance(tile, dict)] != active_tiles:
        fail(check, "materialization tile records are not canonical and exhaustive")
    if any(
        tile.get("phase4_accounting_contract") is not True
        or tile.get("phase5_accounting_contract") is not True
        for tile in tiles
    ):
        fail(check, "a finalized tile lacks a residency or direct-forward contract")

    eligible = require_int(counters, "retained_local_eligible_component_count", check)
    selected = require_int(counters, "retained_local_selected_component_count", check)
    fallback = require_int(counters, "retained_local_fallback_component_count", check)
    if eligible != selected + fallback:
        fail(check, "retained-local eligibility accounting is not exhaustive")
    residency = {
        "contract_status": PASS,
        "activated": selected > 0,
        "contract_tile_count": phase4_tiles,
        "policy_enabled_tile_count": policy_tiles,
        "eligible_component_count": eligible,
        "selected_component_count": selected,
        "fallback_component_count": fallback,
        "selected_route_count": require_int(counters, "retained_local_route_count", check),
        "elided_input_descriptor_count": require_int(
            counters, "elided_retained_input_descriptor_count", check
        ),
        "elided_output_descriptor_count": require_int(
            counters, "elided_retained_output_descriptor_count", check
        ),
    }
    direct_routes = require_int(counters, "direct_forward_selected_route_count", check)
    direct = {
        "contract_status": PASS,
        "activated": direct_routes > 0,
        "contract_tile_count": phase5_tiles,
        "selected_route_count": direct_routes,
        "elided_input_descriptor_count": require_int(
            counters, "direct_forward_elided_input_descriptor_count", check
        ),
        "elided_output_descriptor_count": require_int(
            counters, "direct_forward_elided_output_descriptor_count", check
        ),
    }
    return {
        "status": PASS,
        "active_tile_count": tile_count,
        "epoch_count": epoch_count,
        "maximum_frame_bytes": architecture["fixed_shard_bytes"],
    }, residency, direct


def read_key_value_file(path: Path, check: str) -> dict[str, int]:
    require_nonempty_file(path, check)
    values: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        name, separator, raw = line.partition("=")
        if not separator or name in values or re.fullmatch(r"[a-z_]+", name) is None:
            fail(check, f"malformed key/value evidence in {path}")
        try:
            values[name] = int(raw)
        except ValueError:
            fail(check, f"non-integer key/value evidence in {path}: {name}")
    return values


def validate_abi(
    run: Path, active_tiles: list[int], epoch_count: int
) -> dict[str, Any]:
    check = "abi_preflight"
    summary = read_key_value_file(run / "abi-preflight-summary.txt", check)
    required = {
        "active_tiles",
        "passes",
        "epoch_count",
        "errors",
        "timeout_seconds",
        "parallel_jobs",
    }
    if set(summary) != required:
        fail(check, "ABI preflight summary fields are invalid")
    tile_count = len(active_tiles)
    if (
        summary["active_tiles"] != tile_count
        or summary["passes"] != tile_count
        or summary["epoch_count"] != epoch_count
        or summary["errors"] != 0
        or summary["timeout_seconds"] <= 0
        or summary["parallel_jobs"] <= 0
    ):
        fail(check, "ABI preflight summary is incomplete or contradictory")

    directory = run / "abi-preflight"
    if directory.is_symlink() or not directory.is_dir():
        fail(check, "ABI preflight log directory is missing or symbolic")
    logs: dict[int, Path] = {}
    for path in directory.iterdir():
        match = re.fullmatch(r"tile-([0-9]+)\.log", path.name)
        if match is not None:
            logs[int(match.group(1))] = path
    if set(logs) != set(active_tiles):
        fail(check, "ABI preflight logs do not cover the exact active-tile set")
    error_pattern = re.compile(
        r"SCULPTOR_RA_(?:ABI|SIM)_ERROR|panic|fatal|assert", re.IGNORECASE
    )
    for tile in active_tiles:
        require_nonempty_file(logs[tile], check)
        lines = [line.rstrip("\r") for line in logs[tile].read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()]
        expected = f"SCULPTOR_RA_ABI_PASS tile={tile} epoch_count={epoch_count}"
        if lines.count(expected) != 1 or any(error_pattern.search(line) for line in lines):
            fail(check, f"tile {tile} lacks one clean ABI pass")
    return {
        "status": PASS,
        "active_tiles": tile_count,
        "passes": tile_count,
        "errors": 0,
        "per_tile_timeout_seconds": summary["timeout_seconds"],
    }


def riscv_elf(path: Path, check: str) -> int:
    require_nonempty_file(path, check)
    with path.open("rb") as source:
        header = source.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        fail(check, f"not an ELF image: {path}")
    if header[4] not in (1, 2) or header[5] not in (1, 2):
        fail(check, f"ELF class or byte order is invalid: {path}")
    endian = "<" if header[5] == 1 else ">"
    if struct.unpack(endian + "H", header[18:20])[0] != 243:
        fail(check, f"ELF image is not RISC-V: {path}")
    return path.stat().st_size


def validate_elfs(run: Path, active_tiles: list[int]) -> dict[str, Any]:
    check = "elf_generation"
    paths: dict[int, Path] = {}
    for path in run.glob("tile-*.elf"):
        match = re.fullmatch(r"tile-([0-9]+)\.elf", path.name)
        if match is not None:
            paths[int(match.group(1))] = path
    if set(paths) != set(active_tiles):
        fail(check, "production tile ELFs do not cover the exact active-tile set")
    sizes = [riscv_elf(paths[tile], check) for tile in active_tiles]
    idle_size = riscv_elf(run / "idle.elf", check)
    return {
        "status": PASS,
        "tile_elf_count": len(paths),
        "tile_elf_bytes": sum(sizes),
        "maximum_tile_elf_bytes": max(sizes),
        "idle_elf_bytes": idle_size,
    }


def validate_memory_and_persistence(
    run: Path, active_tiles: list[int], scratchpad_bytes: int
) -> tuple[dict[str, Any], dict[str, Any]]:
    check = "persistent_matrices"
    payload = read_json(run / "memory-reports" / "tile-memory-summary.json", check)
    if payload.get("schema_version") != 1:
        fail(check, "tile-memory summary schema is invalid")
    if payload.get("active_tile_count") != len(active_tiles):
        fail(check, "tile-memory summary active count is incomplete")
    if payload.get("scratchpad_capacity_bytes") != scratchpad_bytes:
        fail(check, "tile-memory capacity disagrees with the architecture")
    gate = payload.get("capacity_gate")
    if not isinstance(gate, dict) or gate.get("status") != PASS or gate.get("errors") != []:
        fail(check, "tile-memory capacity gate did not pass")
    tiles = payload.get("tiles")
    if not isinstance(tiles, dict) or set(tiles) != {str(tile) for tile in active_tiles}:
        fail(check, "tile-memory records do not cover every active tile")

    totals = {field: 0 for field in PERSISTENT_FIELDS}
    for tile in active_tiles:
        record = tiles[str(tile)]
        if not isinstance(record, dict):
            fail(check, f"tile-memory record {tile} is malformed")
        capacity = record.get("capacity")
        if (
            not isinstance(capacity, dict)
            or capacity.get("complete") is not True
            or require_int(capacity, "requiredLocalBytes", check) > scratchpad_bytes
        ):
            fail(check, f"tile {tile} has incomplete or over-capacity memory evidence")
        finalized = record.get("finalized")
        if not isinstance(finalized, dict):
            fail(check, f"tile {tile} lacks finalized memory evidence")
        for field in PERSISTENT_FIELDS:
            totals[field] += require_int(finalized, field, check)
        count = finalized["persistent_matrix_count"]
        if count > 0 and (
            finalized["persistent_matrix_programmed_bytes"] == 0
            or finalized["persistent_matrix_consumer_task_count"] == 0
            or finalized["persistent_matrix_consumer_execution_count"] == 0
            or finalized["persistent_matrix_maximum_last_use_epoch"] == 0
        ):
            fail(check, f"tile {tile} has an incomplete persistent-matrix lifetime")
        if any(
            finalized[field] != 0
            for field in (
                "persistent_matrix_duplicate_setup_count",
                "persistent_matrix_unproven_use_count",
                "persistent_matrix_repeated_setup_count",
            )
        ):
            fail(check, f"tile {tile} has a persistent-matrix proof error")
    return {
        "status": PASS,
        "active_tile_count": len(active_tiles),
        "scratchpad_capacity_bytes": scratchpad_bytes,
    }, {
        "contract_status": PASS,
        "activated": totals["persistent_matrix_count"] > 0,
        "contract_tile_count": len(active_tiles),
        "persistent_matrix_count": totals["persistent_matrix_count"],
        "programmed_bytes": totals["persistent_matrix_programmed_bytes"],
        "consumer_task_count": totals["persistent_matrix_consumer_task_count"],
        "consumer_execution_count": totals[
            "persistent_matrix_consumer_execution_count"
        ],
        "duplicate_setup_count": totals[
            "persistent_matrix_duplicate_setup_count"
        ],
        "unproven_use_count": totals["persistent_matrix_unproven_use_count"],
        "repeated_setup_count": totals[
            "persistent_matrix_repeated_setup_count"
        ],
    }


def one_integer(text: str, name: str, tile: int, check: str) -> int:
    matches = re.findall(re.escape(name) + r"\s*=\s*([0-9]+)\s*:\s*i64", text)
    if len(matches) != 1:
        fail(check, f"tile {tile} does not carry exactly one {name}")
    return int(matches[0])


def one_true(text: str, name: str, tile: int, check: str) -> None:
    matches = re.findall(re.escape(name) + r"\s*=\s*(true|false)", text)
    if matches != ["true"]:
        fail(check, f"tile {tile} does not enable {name}")


def record_integer(record: str, name: str, label: str, check: str) -> int:
    matches = re.findall(re.escape(name) + r"\s*=\s*([0-9]+)\s*:\s*i64", record)
    if len(matches) != 1:
        fail(check, f"{label} does not carry exactly one {name}")
    return int(matches[0])


def incremental_scalar_fields(
    record: str, label: str, check: str
) -> dict[str, int]:
    values = {
        name: record_integer(record, name, label, check)
        for name in SCALAR_INCREMENTAL_FIELDS
    }
    for direction in ("input", "output"):
        descriptors = values[
            f"incremental_over_retained_control_{direction}_descriptor_count"
        ]
        requests = values[
            f"incremental_over_retained_control_{direction}_request_count"
        ]
        byte_count = values[
            f"incremental_over_retained_control_{direction}_bytes"
        ]
        if (descriptors == 0 and (requests != 0 or byte_count != 0)) or (
            descriptors > 0 and byte_count == 0
        ):
            fail(
                check,
                f"{label} has an inconsistent {direction} incremental "
                "physical certificate",
            )
    return values


def validate_tile_contracts(
    run: Path,
    active_tiles: list[int],
    architecture: dict[str, int],
    expected_digital_workers: int,
    expected_scalar_contraction_active: bool,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    check = "optimization_contracts"
    core_directory = run / "cores"
    single_slot = double_slot = residency_records = local_epoch_tasks = 0
    scalar_region_records = 0
    scalar_contracted_counts: set[int] = set()
    scalar_fallback_counts: set[int] = set()
    scalar_removed_kernel_counts: set[int] = set()
    common_digital: tuple[int, int, int, int] | None = None
    for tile in active_tiles:
        path = core_directory / f"core-{tile}-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        scalar_prefix = "sculptor.execution_residency.scalar_"
        if expected_scalar_contraction_active:
            if one_integer(
                text,
                "sculptor.execution_residency.scalar_plan_version",
                tile,
                check,
            ) != SCALAR_PLAN_VERSION or text.count(
                "sculptor.execution_residency.scalar_regions"
            ) != 1:
                fail(check, f"tile {tile} has a stale scalar contraction plan")
            if one_integer(
                text,
                "sculptor.execution_residency.physical_certificate_version",
                tile,
                check,
            ) != PHYSICAL_EXECUTION_RESIDENCY_CERTIFICATE_VERSION:
                fail(
                    check,
                    f"tile {tile} has a stale physical execution-residency "
                    "certificate",
                )
            scalar_contracted_counts.add(
                one_integer(
                    text,
                    "sculptor.execution_residency.scalar_contracted_count",
                    tile,
                    check,
                )
            )
            scalar_fallback_counts.add(
                one_integer(
                    text,
                    "sculptor.execution_residency.scalar_fallback_count",
                    tile,
                    check,
                )
            )
            scalar_removed_kernel_counts.add(
                one_integer(
                    text,
                    "sculptor.execution_residency.scalar_removed_kernel_count",
                    tile,
                    check,
                )
            )
            scalar_records: dict[int, dict[str, int]] = {}
            for record in re.findall(r"\{[^{}]+\}", text):
                if "wrapper_routine_id" not in record:
                    continue
                region = record_integer(record, "region_id", "scalar region", check)
                if (
                    record_integer(record, "schema_version", "scalar region", check)
                    != SCALAR_PLAN_VERSION
                    or region in scalar_records
                ):
                    fail(check, f"tile {tile} has a stale or duplicate scalar region")
                scalar_records[region] = incremental_scalar_fields(
                    record, f"tile {tile} scalar region {region}", check
                )
            physical_regions: dict[int, str] = {}
            for record in re.findall(
                r"\{actual_region_owner_bytes = [^{}]+\}", text
            ):
                region = record_integer(record, "region_id", "physical region", check)
                if (
                    record_integer(
                        record, "schema_version", "physical region", check
                    )
                    != PHYSICAL_EXECUTION_RESIDENCY_CERTIFICATE_VERSION
                    or region in physical_regions
                ):
                    fail(
                        check,
                        f"tile {tile} has a stale or duplicate physical region",
                    )
                physical_regions[region] = record
            for region, early in scalar_records.items():
                late_record = physical_regions.get(region)
                if late_record is None:
                    fail(
                        check,
                        f"tile {tile} scalar region {region} lacks a late physical "
                        "certificate",
                    )
                late = incremental_scalar_fields(
                    late_record, f"tile {tile} physical region {region}", check
                )
                if early != late:
                    fail(
                        check,
                        f"tile {tile} scalar region {region} incremental physical "
                        "certificate drifted",
                    )
            scalar_region_records += len(scalar_records)
        elif scalar_prefix in text:
            fail(
                check,
                f"tile {tile} carries scalar contraction state while disabled",
            )
        if one_integer(text, "sculptor.residency.plan_version", tile, check) != 1:
            fail(check, f"tile {tile} has a stale residency plan")
        for marker in (
            "sculptor.residency.records",
            "sculptor.residency.transitions",
            "sculptor.residency.persistent_matrices",
            "sculptor.residency.persistent_matrix_audit",
            "sculptor.materialization.exact_ram_readiness_certificate",
        ):
            if text.count(marker) != 1:
                fail(check, f"tile {tile} lacks exactly one {marker}")
        if one_integer(
            text, "sculptor.residency.persistent_matrix_version", tile, check
        ) != 1:
            fail(check, f"tile {tile} has a stale persistent-matrix plan")
        one_true(
            text,
            "sculptor.materialization.direct_forward_policy_enabled",
            tile,
            check,
        )
        one_true(
            text,
            "sculptor.materialization.exact_ram_readiness_enabled",
            tile,
            check,
        )
        certificate_mode = re.findall(
            r"sculptor\.materialization\.exact_ram_readiness_certificate\s*=\s*"
            r"\{[^}]*\bmode\s*=\s*\"([^\"]+)\"",
            text,
        )
        if certificate_mode != ["exact_dependencies"]:
            fail(check, f"tile {tile} exact-readiness certificate mode is invalid")

        architecture_values = re.findall(
            r"sculptor\.arch\.streaming\s*=\s*\{\s*fixed_shard_bytes\s*=\s*"
            r"([0-9]+)\s*:\s*i64",
            text,
        )
        if architecture_values != [str(architecture["fixed_shard_bytes"])]:
            fail(check, f"tile {tile} maximum-frame contract disagrees")
        ring_values = [
            int(value)
            for value in re.findall(
                r"#sculptor\.shard_residency<[^>]*?\bringSlots\s*=\s*"
                r"([0-9]+)\s*:\s*i64",
                text,
            )
        ]
        if any(value < 1 or value > architecture["max_in_flight"] for value in ring_values):
            fail(check, f"tile {tile} has an out-of-contract residency ring")
        residency_records += len(ring_values)
        single_slot += ring_values.count(1)
        double_slot += ring_values.count(2)
        local_epoch_tasks += text.count("sculptor.materialization.execution_epoch_id")

        digital = (
            one_integer(
                text, "sculptor.mapping.expanded_digital_operation_count", tile, check
            ),
            one_integer(
                text, "sculptor.mapping.expanded_digital_work_unit_count", tile, check
            ),
            one_integer(
                text,
                "sculptor.mapping.digital_profitability_legal_candidate_count",
                tile,
                check,
            ),
            one_integer(
                text,
                "sculptor.mapping.digital_profitability_accepted_candidate_count",
                tile,
                check,
            ),
        )
        if digital[1] < digital[0] or digital[3] > digital[2]:
            fail(check, "digital-worker expansion accounting is inconsistent")
        if common_digital is None:
            common_digital = digital
        elif digital != common_digital:
            fail(check, "tiles disagree on deployment-wide digital-worker accounting")

    assert common_digital is not None
    if expected_scalar_contraction_active and (
        len(scalar_contracted_counts) != 1
        or len(scalar_fallback_counts) != 1
        or len(scalar_removed_kernel_counts) != 1
    ):
        fail(check, "tiles disagree on deployment-wide scalar accounting")
    scalar_contracted = next(iter(scalar_contracted_counts), 0)
    scalar_fallback = next(iter(scalar_fallback_counts), 0)
    scalar_removed_kernels = next(iter(scalar_removed_kernel_counts), 0)
    if expected_scalar_contraction_active and (
        scalar_region_records != scalar_contracted
    ):
        fail(
            check,
            "scalar contracted count does not equal the complete region record set",
        )
    buffering = {
        "contract_status": PASS,
        "activated": residency_records > 0,
        "double_buffering_activated": double_slot > 0,
        "contract_tile_count": len(active_tiles),
        "residency_record_count": residency_records,
        "single_slot_record_count": single_slot,
        "double_slot_record_count": double_slot,
        "maximum_ring_slots": architecture["max_in_flight"],
    }
    digital_workers = {
        "contract_status": PASS,
        "activated": expected_digital_workers > 1 and common_digital[0] > 0,
        "contract_tile_count": len(active_tiles),
        "configured_worker_limit": expected_digital_workers,
        "expanded_operation_count": common_digital[0],
        "expanded_work_unit_count": common_digital[1],
        "legal_candidate_count": common_digital[2],
        "accepted_candidate_count": common_digital[3],
    }
    exact = {
        "contract_status": PASS,
        "activated": local_epoch_tasks > 0,
        "contract_tile_count": len(active_tiles),
        "synchronization_mode": "exact_dependencies",
        "local_epoch_task_count": local_epoch_tasks,
        "runtime_progress_observed": False,
    }
    return {
        "status": PASS,
        "contract_tile_count": len(active_tiles),
        "feature_contract_count": len(FEATURE_NAMES),
        "scalar_contraction_enabled": expected_scalar_contraction_active,
        "scalar_contracted_region_count": scalar_contracted,
        "scalar_fallback_region_count": scalar_fallback,
        "scalar_removed_kernel_count": scalar_removed_kernels,
        "scalar_region_record_count": scalar_region_records,
    }, buffering, digital_workers, exact


def configuration(args: argparse.Namespace) -> dict[str, Any]:
    execution_residency_mode = {
        "0": "off",
        "off": "off",
        "analyze": "analyze",
        "1": "select",
        "select": "select",
    }.get(args.expected_execution_residency_mode,
          args.expected_execution_residency_mode)
    return {
        "fixed_shard_bytes": args.expected_frame_bytes,
        "supported_frame_bytes": list(SUPPORTED_FRAME_BYTES),
        "max_in_flight": args.expected_max_in_flight,
        "digital_workers": args.expected_digital_workers,
        "execution_residency_mode": execution_residency_mode,
        "execution_residency_maximum_members":
            args.expected_execution_residency_maximum_members,
        "execution_residency_maximum_wave_width":
            args.expected_execution_residency_maximum_wave_width,
        "execution_residency_require_positive_benefit":
            args.expected_execution_residency_require_positive_benefit,
        "contract_scalar_execution_regions":
            args.expected_contract_scalar_execution_regions,
        "retained_local_policy_required": True,
        "direct_forward_policy_required": True,
        "persistent_matrix_policy_required": True,
        "exact_dependencies_required": True,
        "retained_owner_boundary_ids": args.retained_owner_boundary_ids,
        "output_value_validation_required": False,
    }


def configuration_hash(value: dict[str, Any]) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def validate_execution_residency(
    run: Path, args: argparse.Namespace
) -> dict[str, Any]:
    check = "execution_residency_audit"
    audit = run / "execution-residency-audit.json"
    source = run / "deployment" / "04-tensor-fragments.mlir"
    output = run / "deployment" / "04-residency-regions.mlir"
    for path in (audit, source, output):
        require_nonempty_file(path, check)
    validator = PROJECT_ROOT / "scripts" / "validate-sculptor-execution-residency.py"
    require_nonempty_file(validator, check)
    command = [
        sys.executable,
        str(validator),
        "--audit", str(audit),
        "--input-mlir", str(source),
        "--output-mlir", str(output),
        "--expected-mode", args.expected_execution_residency_mode,
        "--expected-maximum-members",
        str(args.expected_execution_residency_maximum_members),
        "--expected-maximum-wave-width",
        str(args.expected_execution_residency_maximum_wave_width),
        "--expected-require-positive-benefit",
        "true" if args.expected_execution_residency_require_positive_benefit
        else "false",
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        fail(check, result.stderr.strip() or "execution-residency validation failed")
    try:
        validation = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail(check, f"execution-residency validator emitted invalid JSON: {error}")
    if not isinstance(validation, dict) or validation.get("status") != PASS:
        fail(check, "execution-residency validator did not emit PASS")
    return {
        "status": PASS,
        "mode": validation.get("mode"),
        "phase": validation.get("phase"),
        "physical_change_count": validation.get("physical_change_count"),
        "configuration_sha256": validation.get("configuration_sha256"),
        "source_ir_sha256": validation.get("source_ir_sha256"),
        "audit_sha256": sha256_file(audit),
    }


def validate(args: argparse.Namespace) -> dict[str, Any]:
    run = args.run_directory.resolve(strict=True)
    if args.expected_network_size <= 0:
        fail("configuration", "expected network size must be positive")
    if args.expected_frame_bytes not in SUPPORTED_FRAME_BYTES:
        fail("configuration", "expected frame must be one of 4--256 KiB")
    if args.expected_max_in_flight < 1 or args.expected_max_in_flight > 8:
        fail("configuration", "expected max-in-flight must be in [1, 8]")
    if args.expected_digital_workers <= 0:
        fail("configuration", "expected digital workers must be positive")
    if args.expected_execution_residency_mode not in {
        "0", "off", "analyze", "1", "select"
    }:
        fail("configuration", "expected execution-residency mode is invalid")
    if args.expected_execution_residency_maximum_members <= 0:
        fail("configuration", "execution-residency maximum members must be positive")
    if not 1 <= args.expected_execution_residency_maximum_wave_width <= 8:
        fail("configuration", "execution-residency maximum wave width must be in [1, 8]")
    if not isinstance(
        args.expected_execution_residency_require_positive_benefit, bool
    ):
        fail(
            "configuration",
            "execution-residency require-positive-benefit is invalid",
        )
    if not isinstance(args.expected_contract_scalar_execution_regions, bool):
        fail("configuration", "scalar contraction setting is invalid")
    if args.retained_owner_boundary_ids:
        fail(
            "configuration",
            "compile qualification forbids a retained-owner boundary allowlist",
        )

    active_tiles = read_active_tiles(run / "active-cores.txt", args.expected_network_size)
    try:
        deployment = load_deployment_manifest(
            run / "deployment-manifest.json",
            network_size=args.expected_network_size,
            expected_active_tiles=active_tiles,
        )
    except (RuntimeError, ValueError) as error:
        fail("exact_dependencies_local_epoch", str(error))
    if deployment["synchronization_mode"] != "exact_dependencies":
        fail(
            "exact_dependencies_local_epoch",
            "compile qualification requires exact_dependencies",
        )

    architecture = validate_architecture(
        run, args.expected_frame_bytes, args.expected_max_in_flight
    )
    lowering = validate_lowering(run, active_tiles)
    execution_residency = validate_execution_residency(run, args)
    materialization, residency, direct = validate_materialization(
        run, active_tiles, architecture, deployment["epoch_count"]
    )
    abi = validate_abi(run, active_tiles, deployment["epoch_count"])
    elfs = validate_elfs(run, active_tiles)
    memory, persistent = validate_memory_and_persistence(
        run, active_tiles, architecture["scratchpad_bytes"]
    )
    expected_scalar_contraction_active = (
        args.expected_contract_scalar_execution_regions
        and args.expected_execution_residency_mode in {"1", "select"}
    )
    contracts, buffering, digital, exact = validate_tile_contracts(
        run,
        active_tiles,
        architecture,
        args.expected_digital_workers,
        expected_scalar_contraction_active,
    )
    exact["semantic_epoch_count"] = deployment["epoch_count"]
    frame = {
        "contract_status": PASS,
        "activated": True,
        "contract_tile_count": len(active_tiles),
        "configured_maximum_frame_bytes": architecture["fixed_shard_bytes"],
        "supported_maximum_frame_bytes": list(SUPPORTED_FRAME_BYTES),
        "observed_maximum_frame_bytes": [architecture["fixed_shard_bytes"]],
    }
    config = configuration(args)
    return {
        "schema": "sculptor.compile-qualification",
        "version": 1,
        "status": PASS,
        "model": args.model,
        "active_tile_ids": active_tiles,
        "configuration": config,
        "configuration_sha256": configuration_hash(config),
        "checks": {
            "complete_lowering": lowering,
            "execution_residency_audit": execution_residency,
            "materialization_audit": materialization,
            "abi_preflight": abi,
            "elf_generation": elfs,
            "memory_validation": memory,
            "optimization_contracts": contracts,
        },
        "features": {
            "residency": residency,
            "direct_forwarding": direct,
            "persistent_matrices": persistent,
            "circular_double_buffering": buffering,
            "digital_workers": digital,
            "exact_dependencies_local_epoch": exact,
            "configurable_frames": frame,
        },
        "output_value_validation_required": False,
        "errors": [],
    }


def write_certificate(path: Path, certificate: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(certificate, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-directory", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--expected-network-size", type=int, required=True)
    parser.add_argument("--expected-frame-bytes", type=int, required=True)
    parser.add_argument("--expected-max-in-flight", type=int, required=True)
    parser.add_argument("--expected-digital-workers", type=int, required=True)
    parser.add_argument("--expected-execution-residency-mode", required=True)
    parser.add_argument(
        "--expected-execution-residency-maximum-members", type=int, required=True
    )
    parser.add_argument(
        "--expected-execution-residency-maximum-wave-width", type=int, required=True
    )
    parser.add_argument(
        "--expected-execution-residency-require-positive-benefit",
        type=lambda value: {"0": False, "false": False,
                            "1": True, "true": True}.get(value),
        required=True,
    )
    parser.add_argument(
        "--expected-contract-scalar-execution-regions",
        type=lambda value: {"0": False, "false": False,
                            "1": True, "true": True}.get(value),
        required=True,
    )
    parser.add_argument("--retained-owner-boundary-ids", default="")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        certificate = validate(args)
    except (QualificationError, OSError, UnicodeError) as error:
        check = error.check if isinstance(error, QualificationError) else "artifact_io"
        config = configuration(args)
        certificate = {
            "schema": "sculptor.compile-qualification",
            "version": 1,
            "status": "FAIL",
            "model": args.model,
            "active_tile_ids": [],
            "configuration": config,
            "configuration_sha256": configuration_hash(config),
            "checks": {},
            "features": {},
            "output_value_validation_required": False,
            "failed_check": check,
            "errors": [str(error)],
        }
        write_certificate(args.output, certificate)
        print(f"compile qualification failed at {check}: {error}", file=sys.stderr)
        return 1
    write_certificate(args.output, certificate)
    features = certificate["features"]
    activated = [name for name in FEATURE_NAMES if features[name]["activated"]]
    print(
        "compile qualification PASS: "
        f"model={args.model} tiles={len(certificate['active_tile_ids'])} "
        f"features={len(features)}/{len(FEATURE_NAMES)} "
        f"activated={','.join(activated) or 'none'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
