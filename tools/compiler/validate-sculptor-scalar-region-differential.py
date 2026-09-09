#!/usr/bin/env python3
"""Validate the FAST Phase-5 scalar-region differential gate.

The control and candidate must be two immutable, same-tree ``select`` builds.
The only workload/compiler difference is scalar-region contraction.  This
validator deliberately joins four independent evidence surfaces: the frozen
pre-outline deployment, the late physical scalar certificate, the aggregate
materialization audit, and three clean SST completions per arm.
"""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re
from statistics import median
import sys
from typing import Any, Iterable


UINT64_MAX = (1 << 64) - 1
INVALID_U32 = (1 << 32) - 1
REQUIRED_STAGES = (
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
OPTIONAL_STAGES = ("03-duplicate-matrices.mlir",)
REQUIRED_QUALIFICATION_CHECKS = (
    "complete_lowering",
    "execution_residency_audit",
    "materialization_audit",
    "abi_preflight",
    "elf_generation",
    "memory_validation",
    "optimization_contracts",
)
SEMANTIC_MATERIALIZATION_COUNTERS = (
    "epoch_count",
    "materialized_consumer_region_count",
    "materialized_producer_region_count",
    "materialized_tensor_count",
    "materialized_tensor_bytes",
    "materialized_main_transfer_count_logical",
    "materialized_tail_transfer_count_logical",
    "retained_local_fallback_component_count",
    "preserved_external_spill_descriptor_count",
    "preserved_hybrid_spill_descriptor_count",
)
ZERO_CORRECTNESS_COUNTERS = (
    "cross_epoch_direct_route_count",
    "deferred_dependency_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "unclassified_boundary_count",
    "unowned_materialized_byte_count",
    "zero_contribution_consumer_region_count",
)
PHYSICAL_FIELDS = {
    "input": {
        "descriptors": "materialized_input_dma_descriptor_count",
        "requests": "materialized_input_physical_request_count",
        "bytes": "materialized_input_physical_byte_count",
    },
    "output": {
        "descriptors": "materialized_output_dma_descriptor_count",
        "requests": "materialized_output_physical_request_count",
        "bytes": "materialized_output_physical_byte_count",
    },
}
SCALAR_PRE_FIELDS = {
    "input": {
        "descriptors": "pre_contraction_input_descriptor_count",
        "requests": "pre_contraction_input_request_count",
        "bytes": "pre_contraction_input_bytes",
    },
    "output": {
        "descriptors": "pre_contraction_output_descriptor_count",
        "requests": "pre_contraction_output_request_count",
        "bytes": "pre_contraction_output_bytes",
    },
}
SCALAR_LATE_FIELDS = {
    "input": {
        "descriptors": "elided_input_descriptor_count",
        "requests": "elided_input_request_count",
        "bytes": "elided_input_bytes",
    },
    "output": {
        "descriptors": "elided_output_descriptor_count",
        "requests": "elided_output_request_count",
        "bytes": "elided_output_bytes",
    },
}
SCALAR_INCREMENTAL_FIELDS = {
    "input": {
        "descriptors": "incremental_over_retained_control_input_descriptor_count",
        "requests": "incremental_over_retained_control_input_request_count",
        "bytes": "incremental_over_retained_control_input_bytes",
    },
    "output": {
        "descriptors": "incremental_over_retained_control_output_descriptor_count",
        "requests": "incremental_over_retained_control_output_request_count",
        "bytes": "incremental_over_retained_control_output_bytes",
    },
}
DESCRIPTOR_IDENTITY_FIELDS = (
    "operationId",
    "epochId",
    "workUnitId",
    "tensorId",
    "portNumber",
    "loopId",
    "direction",
    "templateKind",
    "flags",
    "iterationBegin",
    "iterationEnd",
    "iterationStep",
    "segmentOffset",
    "segmentCount",
    "bytesPerIteration",
    "boundaryId",
)
SURVIVING_DESCRIPTOR_FIELDS = (
    "tensorId",
    "direction",
    "templateKind",
    "flags",
    "iterationBegin",
    "iterationEnd",
    "iterationStep",
    "segmentCount",
    "bytesPerIteration",
    "boundaryId",
)
STOP_PATTERN = re.compile(
    r"MITTENS_QEMU_CAPTURE_HOST stop_reason=([^\s(]+)\([0-9]+\) "
    r"count=([0-9]+)"
)
PROGRESS_FIELD_PATTERN = re.compile(r"([a-zA-Z0-9_]+)=([^ ]+)")


class DifferentialError(RuntimeError):
    """Missing, malformed, stale, or contradictory Phase-5 evidence."""


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
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read {label}: {error}")
    if not isinstance(value, dict):
        fail(f"{label} must contain one JSON object")
    return value


def read_one_csv_row(path: Path, label: str) -> dict[str, str]:
    path = require_file(path, label)
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        rows = list(reader)
    if reader.fieldnames is None or len(rows) != 1 or None in rows[0]:
        fail(f"{label} must contain one complete row")
    return rows[0]


def require_u64(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= UINT64_MAX:
        fail(f"{label} must be an unsigned 64-bit integer")
    return value


def require_counter(counters: dict[str, Any], name: str, label: str) -> int:
    if name not in counters:
        fail(f"{label} lacks counter {name}")
    return require_u64(counters[name], f"{label}.{name}")


def integer_field(
    record: str, name: str, label: str, *, allow_negative: bool = False
) -> int:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*(-?[0-9]+)\s*:\s*i64\b", record)
    if match is None:
        fail(f"{label} lacks {name}")
    value = int(match.group(1))
    if value < 0 and not allow_negative:
        fail(f"{label}.{name} is negative")
    return value


def integer_array(record: str, name: str, label: str) -> list[int]:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\[([^\]]*)\]", record)
    if match is None:
        fail(f"{label} lacks {name}")
    raw = match.group(1).strip()
    if not raw:
        return []
    try:
        values = [int(value.strip()) for value in raw.split(",")]
    except ValueError:
        fail(f"{label}.{name} is not an integer array")
    if any(value < 0 for value in values):
        fail(f"{label}.{name} contains a negative value")
    return values


def active_tiles(run: Path) -> list[int]:
    path = require_file(run / "active-cores.txt", "active-core manifest")
    try:
        values = [int(line) for line in path.read_text(encoding="utf-8").splitlines()]
    except ValueError as error:
        fail(f"active-core manifest is malformed: {error}")
    if not values or values != sorted(set(values)) or values[0] < 0:
        fail("active-core manifest is empty, duplicated, or noncanonical")
    return values


def normalized_environment(manifest: dict[str, Any]) -> dict[str, Any]:
    environment = manifest.get("environment")
    if not isinstance(environment, dict):
        fail("run manifest lacks environment")
    result = dict(environment)
    for name in (
        "GOLEM_MODEL_OUTPUT_DIR",
        "GOLEM_MODEL_RUN_ID",
        "GOLEM_MODEL_CONTRACT_SCALAR_EXECUTION_REGIONS",
    ):
        result.pop(name, None)
    return result


def normalized_parameters(manifest: dict[str, Any]) -> dict[str, Any]:
    parameters = manifest.get("parameters")
    if not isinstance(parameters, dict):
        fail("run manifest lacks parameters")
    result = dict(parameters)
    result.pop("contract_scalar_execution_regions", None)
    return result


def scalar_setting(manifest: dict[str, Any], qualification: dict[str, Any]) -> bool:
    parameters = manifest.get("parameters", {})
    environment = manifest.get("environment", {})
    configuration = qualification.get("configuration", {})
    values = (
        parameters.get("contract_scalar_execution_regions"),
        environment.get("GOLEM_MODEL_CONTRACT_SCALAR_EXECUTION_REGIONS"),
        configuration.get("contract_scalar_execution_regions"),
    )

    def convert(value: Any) -> bool | None:
        if value in (True, 1, "1", "true", "True"):
            return True
        if value in (False, 0, "0", "false", "False"):
            return False
        return None

    converted = [convert(value) for value in values]
    if any(value is None for value in converted) or len(set(converted)) != 1:
        fail("scalar contraction setting is missing or disagrees across evidence")
    return bool(converted[0])


def artifact_binding(manifest: dict[str, Any], name: str, path: Path) -> None:
    artifacts = manifest.get("artifacts")
    record = artifacts.get(name) if isinstance(artifacts, dict) else None
    if not isinstance(record, dict) or record.get("sha256") != sha256(path):
        fail(f"run manifest is not bound to {name}")


def streaming_architecture_semantics(run: Path) -> tuple[dict[str, Any], str]:
    """Return the path-independent architecture contract for one compile.

    The manifest deliberately records the absolute path of the placed MLIR
    that produced it.  Two append-only A/B compile directories therefore
    cannot have byte-identical manifests even when their placed IR and
    architecture are identical.  Validate that provenance path against each
    run locally, then compare and hash only the semantic contract.
    """
    path = run / "streaming-architecture.json"
    manifest = read_json(path, "streaming architecture manifest")
    if (
        manifest.get("schema") != "golem.streaming-architecture"
        or manifest.get("schema_version") != 1
    ):
        fail("streaming architecture manifest has a stale schema")
    source_mlir = manifest.get("source_mlir")
    if not isinstance(source_mlir, str) or not source_mlir:
        fail("streaming architecture manifest lacks source_mlir")
    expected_source = (run / "deployment" / "08-placed.mlir").resolve()
    if Path(source_mlir).resolve() != expected_source:
        fail("streaming architecture manifest is not bound to its placed MLIR")
    architecture = manifest.get("architecture")
    if not isinstance(architecture, dict) or not architecture:
        fail("streaming architecture manifest lacks an architecture contract")
    semantics = {
        "schema": manifest["schema"],
        "schema_version": manifest["schema_version"],
        "architecture": architecture,
    }
    digest = hashlib.sha256(
        json.dumps(semantics, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    ).hexdigest()
    return semantics, digest


def extract_scalar_evidence(
    run: Path, tiles: list[int], enabled: bool
) -> dict[str, Any]:
    scalar_records: dict[int, str] = {}
    physical_regions: dict[int, str] = {}
    physical_edges: dict[tuple[int, int], str] = {}
    descriptors: Counter[tuple[int, ...]] = Counter()
    global_counts: set[tuple[int, int, int]] = set()
    scalar_prefix = "sculptor.execution_residency.scalar_"

    for tile in tiles:
        path = require_file(run / "cores" / f"core-{tile}-finalized.mlir", f"tile {tile} finalized MLIR")
        text = path.read_text(encoding="utf-8")
        if (
            integer_field(
                text,
                "sculptor.execution_residency.physical_certificate_version",
                f"tile {tile}",
            )
            != 2
        ):
            fail(f"tile {tile} has a stale physical execution-residency certificate")
        if enabled:
            version = integer_field(text, scalar_prefix + "plan_version", f"tile {tile}")
            if version != 3 or text.count(scalar_prefix + "regions") != 1:
                fail(f"tile {tile} has a stale scalar plan")
            global_counts.add(
                (
                    integer_field(text, scalar_prefix + "contracted_count", f"tile {tile}"),
                    integer_field(text, scalar_prefix + "fallback_count", f"tile {tile}"),
                    integer_field(text, scalar_prefix + "removed_kernel_count", f"tile {tile}"),
                )
            )
        elif scalar_prefix in text:
            fail(f"control tile {tile} carries scalar contraction state")

        for record in re.findall(r"\{[^{}]+\}", text):
            if "wrapper_routine_id" not in record:
                continue
            region = integer_field(record, "region_id", "scalar region")
            if integer_field(record, "schema_version", "scalar region") != 3:
                fail(f"scalar region {region} has a stale schema")
            if region in scalar_records:
                fail(f"duplicate scalar region certificate {region}")
            scalar_records[region] = record
        for record in re.findall(r"\{actual_region_owner_bytes = [^{}]+\}", text):
            region = integer_field(record, "region_id", "physical region")
            if integer_field(record, "schema_version", "physical region") != 2:
                fail(f"physical region {region} has a stale schema")
            if region in physical_regions:
                fail(f"duplicate physical region certificate {region}")
            physical_regions[region] = record
        for record in re.findall(r"\{boundary_ids = [^{}]+\}", text):
            if "disposition = " not in record:
                continue
            key = (
                integer_field(record, "region_id", "physical edge"),
                integer_field(record, "edge_ordinal", "physical edge"),
            )
            if integer_field(record, "schema_version", "physical edge") != 2:
                fail(f"physical edge {key} has a stale schema")
            if key in physical_edges:
                fail(f"duplicate physical edge certificate {key}")
            physical_edges[key] = record
        for record in re.findall(r"#sculptor\.materialized_dma_descriptor<[^>]+>", text):
            identity = tuple(
                integer_field(
                    record,
                    field,
                    f"tile {tile} DMA descriptor",
                    allow_negative=field == "loopId",
                )
                for field in DESCRIPTOR_IDENTITY_FIELDS
            )
            descriptors[identity] += 1

    if enabled:
        if len(global_counts) != 1:
            fail("candidate tiles disagree on deployment-wide scalar counts")
        contracted, fallback, removed = next(iter(global_counts))
    else:
        contracted = fallback = removed = 0
        if scalar_records:
            fail("control carries scalar region certificates")

    return {
        "contracted": contracted,
        "fallback": fallback,
        "removed_kernels": removed,
        "scalar_records": scalar_records,
        "physical_regions": physical_regions,
        "physical_edges": physical_edges,
        "descriptors": descriptors,
    }


def load_compile(run_path: Path, expected_scalar: bool) -> dict[str, Any]:
    label = "candidate" if expected_scalar else "control"
    run = require_directory(run_path, f"{label} compile")
    qualification_path = run / "compile-qualification.json"
    qualification = read_json(qualification_path, f"{label} compile qualification")
    if qualification.get("schema") != "sculptor.compile-qualification" or qualification.get("status") != "PASS":
        fail(f"{label} compile qualification is not PASS")
    checks = qualification.get("checks")
    if not isinstance(checks, dict):
        fail(f"{label} compile qualification lacks checks")
    for name in REQUIRED_QUALIFICATION_CHECKS:
        check = checks.get(name)
        if not isinstance(check, dict) or check.get("status") != "PASS":
            fail(f"{label} compile qualification check {name} is not PASS")

    manifest = read_json(run / "run-manifest.json", f"{label} run manifest")
    run_record = manifest.get("run")
    if not isinstance(run_record, dict) or run_record.get("mode") != "compile" or not run_record.get("model"):
        fail(f"{label} run manifest is not a named compile")
    if qualification.get("model") != run_record["model"]:
        fail(f"{label} qualification model disagrees with its run manifest")
    if scalar_setting(manifest, qualification) is not expected_scalar:
        fail(f"{label} scalar contraction setting is wrong")
    parameters = manifest.get("parameters", {})
    if parameters.get("execution_residency_mode") not in ("1", "select"):
        fail(f"{label} is not an execution-residency select build")

    tiles = active_tiles(run)
    deployment = read_json(run / "deployment-manifest.json", f"{label} deployment manifest")
    if deployment.get("schema") != "sculptor.deployment" or deployment.get("active_tile_ids") != tiles:
        fail(f"{label} deployment manifest has a stale active-tile set")
    if qualification.get("active_tile_ids") != tiles:
        fail(f"{label} qualification has a stale active-tile set")

    for tile in tiles:
        require_file(run / f"tile-{tile}.elf", f"{label} tile {tile} ELF")
    require_file(run / "abi-preflight-summary.txt", f"{label} ABI summary")

    audit_path = run / "execution-residency-audit.json"
    audit = read_json(audit_path, f"{label} execution-residency audit")
    if (
        audit.get("schema") != "sculptor.execution-residency-audit"
        or audit.get("status") != "PASS"
        or audit.get("mode") != "select"
        or audit.get("phase") != 3
        or require_u64(audit.get("selected_region_count"), "selected_region_count") == 0
        or require_u64(audit.get("selected_member_count"), "selected_member_count") == 0
        or require_u64(audit.get("selected_internal_edge_count"), "selected_internal_edge_count") == 0
    ):
        fail(f"{label} selected residency audit is stale or empty")

    materialization_path = run / "materialization-audit.json"
    materialization = read_json(materialization_path, f"{label} materialization audit")
    if (
        materialization.get("schema") != "sculptor.materialization-audit"
        or materialization.get("status") != "PASS"
        or materialization.get("active_tile_ids") != tiles
        or materialization.get("errors") != []
    ):
        fail(f"{label} materialization audit is incomplete")
    counters = materialization.get("counters")
    if not isinstance(counters, dict):
        fail(f"{label} materialization audit lacks counters")
    for name in ZERO_CORRECTNESS_COUNTERS:
        if require_counter(counters, name, label) != 0:
            fail(f"{label} materialization has nonzero {name}")

    artifact_binding(manifest, "compile_qualification", qualification_path)
    artifact_binding(manifest, "execution_residency_audit", audit_path)
    artifact_binding(manifest, "materialization_audit", materialization_path)
    scalar = extract_scalar_evidence(run, tiles, expected_scalar)
    optimization = checks["optimization_contracts"]
    expected_qualification = {
        "scalar_contraction_enabled": expected_scalar,
        "scalar_contracted_region_count": scalar["contracted"],
        "scalar_fallback_region_count": scalar["fallback"],
        "scalar_removed_kernel_count": scalar["removed_kernels"],
        "scalar_region_record_count": len(scalar["scalar_records"]),
    }
    for name, expected in expected_qualification.items():
        if optimization.get(name) != expected:
            fail(
                f"{label} qualification {name} does not reconcile with "
                "finalized scalar evidence"
            )
    return {
        "directory": str(run),
        "model": run_record["model"],
        "tiles": tiles,
        "qualification": qualification,
        "deployment": deployment,
        "manifest": manifest,
        "manifest_sha256": sha256(run / "run-manifest.json"),
        "audit": audit,
        "audit_sha256": sha256(audit_path),
        "materialization_sha256": sha256(materialization_path),
        "counters": counters,
        "scalar": scalar,
    }


def compare_frozen_inputs(control: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    if control["model"] != candidate["model"] or control["tiles"] != candidate["tiles"]:
        fail("control and candidate model or active-tile set differs")
    for name in ("hardware", "source_trees", "tools"):
        if control["manifest"].get(name) != candidate["manifest"].get(name):
            fail(f"control and candidate {name} differ")
    if normalized_environment(control["manifest"]) != normalized_environment(candidate["manifest"]):
        fail("compile environments differ beyond scalar contraction and run paths")
    if normalized_parameters(control["manifest"]) != normalized_parameters(candidate["manifest"]):
        fail("compile parameters differ beyond scalar contraction")
    control_configuration = dict(control["qualification"].get("configuration", {}))
    candidate_configuration = dict(candidate["qualification"].get("configuration", {}))
    control_configuration.pop("contract_scalar_execution_regions", None)
    candidate_configuration.pop("contract_scalar_execution_regions", None)
    if control_configuration != candidate_configuration:
        fail("compile qualification configurations differ beyond scalar contraction")
    if control["deployment"] != candidate["deployment"]:
        fail("control and candidate deployment manifests differ")
    if control["audit_sha256"] != candidate["audit_sha256"]:
        fail("control and candidate selected residency audits differ")

    files: dict[str, str] = {}
    for relative in ("model.mlir",):
        left = Path(control["directory"]) / relative
        right = Path(candidate["directory"]) / relative
        if sha256(left) != sha256(right):
            fail(f"control and candidate {relative} differ")
        files[relative] = sha256(left)
    control_architecture, architecture_sha256 = streaming_architecture_semantics(
        Path(control["directory"])
    )
    candidate_architecture, candidate_architecture_sha256 = (
        streaming_architecture_semantics(Path(candidate["directory"]))
    )
    if (
        control_architecture != candidate_architecture
        or architecture_sha256 != candidate_architecture_sha256
    ):
        fail("control and candidate streaming architecture contracts differ")
    files["streaming-architecture.json#semantic-contract"] = architecture_sha256
    for name in REQUIRED_STAGES:
        left = Path(control["directory"]) / "deployment" / name
        right = Path(candidate["directory"]) / "deployment" / name
        if sha256(left) != sha256(right):
            fail(f"control and candidate pre-outline stage {name} differs")
        files[f"deployment/{name}"] = sha256(left)
    for name in OPTIONAL_STAGES:
        left = Path(control["directory"]) / "deployment" / name
        right = Path(candidate["directory"]) / "deployment" / name
        if left.exists() != right.exists():
            fail(f"control and candidate optional stage set differs at {name}")
        if left.exists():
            if sha256(left) != sha256(right):
                fail(f"control and candidate pre-outline stage {name} differs")
            files[f"deployment/{name}"] = sha256(left)
    return {"model": control["model"], "active_tile_count": len(control["tiles"]), "identical_artifact_sha256": files}


def validate_scalar_certificate(candidate: dict[str, Any]) -> dict[str, Any]:
    scalar = candidate["scalar"]
    records: dict[int, str] = scalar["scalar_records"]
    late: dict[int, str] = scalar["physical_regions"]
    edges: dict[tuple[int, int], str] = scalar["physical_edges"]
    if scalar["contracted"] <= 0 or len(records) != scalar["contracted"]:
        fail("scalar contracted count does not equal the exact region record count")
    selected = require_u64(candidate["audit"].get("selected_region_count"), "selected regions")
    if scalar["contracted"] + scalar["fallback"] != selected:
        fail("scalar contracted/fallback counts do not cover selected regions")

    structural_totals = {
        direction: {metric: 0 for metric in fields}
        for direction, fields in SCALAR_PRE_FIELDS.items()
    }
    incremental_totals = {
        direction: {metric: 0 for metric in fields}
        for direction, fields in SCALAR_INCREMENTAL_FIELDS.items()
    }
    removed_kernels = 0
    internal_boundaries: set[int] = set()
    internal_edge_count = 0
    for region, record in records.items():
        if region not in late:
            fail(f"scalar region {region} lacks a late physical certificate")
        late_record = late[region]
        if integer_field(late_record, "member_task_count", f"region {region}") != 1:
            fail(f"scalar region {region} was not lowered to one task")
        edge_count = integer_field(late_record, "internal_edge_count", f"region {region}")
        if edge_count <= 0 or integer_field(late_record, "ssa_internal_edge_count", f"region {region}") != edge_count:
            fail(f"scalar region {region} is not wholly SSA-internal")
        for name in (
            "owner_alias_edge_count",
            "materialized_shadow_edge_count",
            "explicit_assembly_edge_count",
            "explicit_reduction_edge_count",
        ):
            if integer_field(late_record, name, f"region {region}") != 0:
                fail(f"scalar region {region} retains internal physical disposition {name}")

        boundaries = integer_array(record, "internal_boundary_ids", f"scalar region {region}")
        if len(boundaries) != edge_count or len(boundaries) != len(set(boundaries)):
            fail(f"scalar region {region} internal boundary set is incomplete")
        if internal_boundaries.intersection(boundaries):
            fail("scalar internal boundary IDs are not deployment-unique")
        internal_boundaries.update(boundaries)
        region_edges = [value for (edge_region, _), value in edges.items() if edge_region == region]
        if len(region_edges) != edge_count:
            fail(f"scalar region {region} edge certificate count disagrees")
        certified_boundaries: set[int] = set()
        for edge in region_edges:
            disposition = re.search(r'\bdisposition\s*=\s*"([^"]+)"', edge)
            if disposition is None or disposition.group(1) != "ssa_internal":
                fail(f"scalar region {region} has a non-SSA internal edge")
            for name in (
                "fill_descriptor_count", "fill_request_count", "fill_bytes",
                "spill_descriptor_count", "spill_request_count", "spill_bytes",
            ):
                if integer_field(edge, name, f"scalar region {region} edge") != 0:
                    fail(f"scalar region {region} has nonzero internal physical RAM/NoC work")
            certified_boundaries.update(integer_array(edge, "boundary_ids", f"scalar region {region} edge"))
        if certified_boundaries != set(boundaries):
            fail(f"scalar region {region} late edge identities disagree")

        for direction in ("input", "output"):
            for metric in ("descriptors", "requests", "bytes"):
                pre = integer_field(record, SCALAR_PRE_FIELDS[direction][metric], f"scalar region {region}")
                observed = integer_field(late_record, SCALAR_LATE_FIELDS[direction][metric], f"region {region}")
                if pre != observed:
                    fail(f"scalar region {region} {direction} {metric} elision certificate drifted")
                structural_totals[direction][metric] += pre
                incremental = integer_field(
                    record,
                    SCALAR_INCREMENTAL_FIELDS[direction][metric],
                    f"scalar region {region}",
                )
                late_incremental = integer_field(
                    late_record,
                    SCALAR_INCREMENTAL_FIELDS[direction][metric],
                    f"region {region}",
                )
                if incremental != late_incremental:
                    fail(
                        f"scalar region {region} {direction} {metric} "
                        "incremental-over-retained-control certificate drifted"
                    )
                incremental_totals[direction][metric] += incremental
            descriptors = integer_field(
                record,
                SCALAR_INCREMENTAL_FIELDS[direction]["descriptors"],
                f"scalar region {region}",
            )
            requests = integer_field(
                record,
                SCALAR_INCREMENTAL_FIELDS[direction]["requests"],
                f"scalar region {region}",
            )
            byte_count = integer_field(
                record,
                SCALAR_INCREMENTAL_FIELDS[direction]["bytes"],
                f"scalar region {region}",
            )
            if (descriptors == 0 and (requests != 0 or byte_count != 0)) or (
                descriptors > 0 and byte_count == 0
            ):
                fail(
                    f"scalar region {region} has an inconsistent {direction} "
                    "incremental-over-retained-control certificate"
                )
        expected_total_descriptors = structural_totals_for_record(record, "descriptor_count")
        expected_total_requests = structural_totals_for_record(record, "request_count")
        expected_total_bytes = structural_totals_for_record(record, "bytes")
        if integer_field(late_record, "elided_descriptor_count", f"region {region}") != expected_total_descriptors:
            fail(f"scalar region {region} total descriptor elision is inconsistent")
        if integer_field(late_record, "elided_request_count", f"region {region}") != expected_total_requests:
            fail(f"scalar region {region} total request elision is inconsistent")
        if integer_field(late_record, "elided_bytes", f"region {region}") != expected_total_bytes:
            fail(f"scalar region {region} total byte elision is inconsistent")
        removed_kernels += integer_field(record, "removed_kernel_count", f"scalar region {region}")
        internal_edge_count += edge_count
    if removed_kernels != scalar["removed_kernels"]:
        fail("scalar removed-kernel total does not reconcile")
    return {
        "contracted_region_count": scalar["contracted"],
        "fallback_region_count": scalar["fallback"],
        "removed_kernel_count": scalar["removed_kernels"],
        "internal_edge_count": internal_edge_count,
        "internal_boundary_count": len(internal_boundaries),
        "internal_boundary_ids": sorted(internal_boundaries),
        "certified_structural_elision": structural_totals,
        "certified_incremental_over_retained_control": incremental_totals,
    }


def structural_totals_for_record(record: str, suffix: str) -> int:
    return integer_field(record, f"pre_contraction_input_{suffix}", "scalar record") + integer_field(record, f"pre_contraction_output_{suffix}", "scalar record")


def semantic_descriptor_counter(
    descriptors: Counter[tuple[int, ...]],
) -> Counter[tuple[int, ...]]:
    """Remove task-table identities that contraction must regenerate.

    Operation, work-unit, epoch, port, loop, and segment-table offsets are
    implementation identities of the outlined runtime graph.  Scalar
    contraction intentionally rebuilds them.  Tensor, direction, transfer
    geometry, flags, and boundary identity are the stable physical action.
    """
    indexes = tuple(
        DESCRIPTOR_IDENTITY_FIELDS.index(name)
        for name in SURVIVING_DESCRIPTOR_FIELDS
    )
    result: Counter[tuple[int, ...]] = Counter()
    for identity, count in descriptors.items():
        result[tuple(identity[index] for index in indexes)] += count
    return result


def compare_compile_work(control: dict[str, Any], candidate: dict[str, Any], certificate: dict[str, Any]) -> dict[str, Any]:
    semantic: dict[str, int] = {}
    for name in SEMANTIC_MATERIALIZATION_COUNTERS:
        left = require_counter(control["counters"], name, "control")
        right = require_counter(candidate["counters"], name, "candidate")
        if left != right:
            fail(f"semantic/cut materialization counter differs: {name}")
        semantic[name] = left

    physical: dict[str, Any] = {}
    for direction in ("input", "output"):
        physical[direction] = {}
        for metric, counter_name in PHYSICAL_FIELDS[direction].items():
            left = require_counter(control["counters"], counter_name, "control")
            right = require_counter(candidate["counters"], counter_name, "candidate")
            delta = left - right
            certified = certificate[
                "certified_incremental_over_retained_control"
            ][direction][metric]
            if delta != certified:
                fail(
                    f"observed {direction} {metric} delta {delta} does not equal "
                    f"scalar certificate {certified}"
                )
            physical[direction][metric] = {"control": left, "candidate": right, "elided": delta}

    control_requests = sum(physical[direction]["requests"]["control"] for direction in ("input", "output"))
    candidate_requests = sum(physical[direction]["requests"]["candidate"] for direction in ("input", "output"))
    control_bytes = sum(physical[direction]["bytes"]["control"] for direction in ("input", "output"))
    candidate_bytes = sum(physical[direction]["bytes"]["candidate"] for direction in ("input", "output"))
    if not (candidate_requests < control_requests and candidate_bytes < control_bytes):
        fail("candidate physical RAM requests and bytes are not both strictly lower")

    control_descriptors = semantic_descriptor_counter(
        control["scalar"]["descriptors"]
    )
    candidate_descriptors = semantic_descriptor_counter(
        candidate["scalar"]["descriptors"]
    )
    unexpected = candidate_descriptors - control_descriptors
    if unexpected:
        fail("candidate introduced or changed a surviving cut/protected DMA descriptor")
    removed = control_descriptors - candidate_descriptors
    certified_descriptor_count = sum(
        certificate["certified_incremental_over_retained_control"][direction][
            "descriptors"
        ]
        for direction in ("input", "output")
    )
    if sum(removed.values()) != certified_descriptor_count:
        fail("surviving descriptor identity delta does not equal the scalar certificate")
    field_index = {
        name: SURVIVING_DESCRIPTOR_FIELDS.index(name)
        for name in SURVIVING_DESCRIPTOR_FIELDS
    }
    boundary_index = field_index["boundaryId"]
    direction_index = field_index["direction"]
    flags_index = field_index["flags"]
    internal_boundaries = set(certificate["internal_boundary_ids"])
    geometry_fields = (
        "tensorId",
        "iterationBegin",
        "iterationEnd",
        "iterationStep",
        "segmentCount",
        "bytesPerIteration",
    )
    geometry_indexes = tuple(field_index[name] for name in geometry_fields)
    internal_geometries = {
        tuple(identity[index] for index in geometry_indexes)
        for identity in control_descriptors
        if identity[direction_index] == 0
        and identity[boundary_index] in internal_boundaries
    }
    for identity, count in control_descriptors.items():
        geometry = tuple(identity[index] for index in geometry_indexes)
        certified_internal = (
            (identity[direction_index] == 0
             and identity[boundary_index] in internal_boundaries)
            or (identity[direction_index] == 1
                and geometry in internal_geometries)
        )
        protected = (identity[flags_index] & 0b11) != 0
        if (protected or not certified_internal) and candidate_descriptors[identity] != count:
            fail("candidate removed or changed cut/protected physical work")
    for region in candidate["scalar"]["scalar_records"]:
        control_region = control["scalar"]["physical_regions"].get(region)
        candidate_region = candidate["scalar"]["physical_regions"].get(region)
        if control_region is None or candidate_region is None:
            fail(f"control/candidate physical certificate lacks scalar region {region}")
        if integer_field(control_region, "protected_spill_count", f"control region {region}") != integer_field(
            candidate_region, "protected_spill_count", f"candidate region {region}"
        ):
            fail(f"scalar region {region} changed protected spill work")
    return {
        "semantic_counters": semantic,
        "physical_ram": physical,
        "total_requests": {"control": control_requests, "candidate": candidate_requests},
        "total_bytes": {"control": control_bytes, "candidate": candidate_bytes},
        "preserved_descriptor_identity_count": sum(candidate_descriptors.values()),
        "removed_descriptor_identity_count": sum(removed.values()),
    }


def parse_simulated_ns(value: str) -> float:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?) (ps|ns|us|ms|s)", value)
    if match is None:
        fail(f"invalid simulated time {value!r}")
    return float(match.group(1)) * {"ps": 1e-3, "ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}[match.group(2)]


def router_summary(path: Path) -> dict[str, int]:
    totals: dict[str, int] = {}
    with require_file(path, "router statistics").open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            if row.get("ComponentName") != "global_ram":
                continue
            try:
                totals[str(row["StatisticName"])] = int(row["Sum.u64"])
            except (KeyError, TypeError, ValueError):
                fail("router statistics contain a malformed global-RAM row")
    for name in ("requests", "bytes"):
        if name not in totals:
            fail(f"router statistics lack global_ram.{name}")
    return totals


def load_sst(path: Path, compile_data: dict[str, Any]) -> dict[str, Any]:
    evidence = require_directory(path, "SST evidence")
    result = read_one_csv_row(evidence / "result.csv", "SST result")
    status = read_one_csv_row(evidence / "status.csv", "SST status")
    if result.get("status") != "PASS" or status.get("status") != "PASS" or status.get("exit_code") != "0":
        fail(f"SST did not terminate successfully: {evidence}")
    epoch_count = require_counter(compile_data["counters"], "epoch_count", "compile")
    if (
        result.get("model") != compile_data["model"]
        or result.get("active_tiles") != str(len(compile_data["tiles"]))
        or result.get("epoch_count") != str(epoch_count)
        or result.get("synchronization_mode") != "exact_dependencies"
    ):
        fail(f"SST result is stale or bound to another workload: {evidence}")
    simulated_ns = parse_simulated_ns(result.get("simulated_time", ""))
    try:
        wall_seconds = float(result["simulation_wall_seconds"])
    except (KeyError, ValueError):
        fail(f"SST result has an invalid wall time: {evidence}")
    if simulated_ns <= 0 or wall_seconds <= 0:
        fail(f"SST reports nonpositive completion time: {evidence}")

    launch = read_json(evidence / "launch.json", "SST launch")
    if (
        launch.get("source_compile_directory") != compile_data["directory"]
        or launch.get("source_run_manifest_sha256") != compile_data["manifest_sha256"]
        or launch.get("materialization_audit_sha256") != compile_data["materialization_sha256"]
    ):
        fail(f"SST launch is not cryptographically bound to its compile: {evidence}")
    launch_work = launch.get("physical_global_dma_work")
    if not isinstance(launch_work, dict) or (
        launch_work.get("input_requests")
        != require_counter(
            compile_data["counters"],
            PHYSICAL_FIELDS["input"]["requests"],
            "compile",
        )
        or launch_work.get("output_requests")
        != require_counter(
            compile_data["counters"],
            PHYSICAL_FIELDS["output"]["requests"],
            "compile",
        )
    ):
        fail(f"SST launch physical-work partition is stale: {evidence}")

    log_path = require_file(evidence / "simulation.log", "SST terminal log")
    log = log_path.read_text(encoding="utf-8", errors="replace")
    tiles = compile_data["tiles"]
    init = sorted(int(value) for value in re.findall(r"SCULPTOR_RA_INIT_PASS tile=([0-9]+)", log))
    if init != tiles or log.count("SCULPTOR_RA_SIM_PASS") != len(tiles) or "SCULPTOR_RA_SIM_FAIL" in log or "Simulation failed" in log:
        fail(f"SST terminal tile accounting is incomplete: {evidence}")

    terminal: dict[int, dict[str, str]] = {}
    for line in log.splitlines():
        if line.startswith("SCULPTOR_RA_PROGRESS kind=2 "):
            fields = dict(PROGRESS_FIELD_PATTERN.findall(line))
            if "tile" not in fields:
                fail("terminal progress row lacks tile")
            terminal[int(fields["tile"])] = fields
    if sorted(terminal) != tiles:
        fail(f"SST lacks one terminal progress row per tile: {evidence}")
    required = {
        "issued", "retired", "physical_global_ram_dma_submitted",
        "physical_global_ram_dma_completed", "current_epoch",
        "earliest_incomplete_epoch", "active_count", "active_loop",
        "next_issue", "ready_queue", "pending_receive", "pending_transmit",
        "pending_dma", "active_receive_states", "reported_receive_states",
    }
    for tile, fields in terminal.items():
        if not required.issubset(fields):
            fail(f"tile {tile} terminal row lacks required fields")
        if (
            int(fields["current_epoch"]) != epoch_count
            or int(fields["earliest_incomplete_epoch"]) != INVALID_U32
            or int(fields["active_loop"]) != INVALID_U32
            or int(fields["next_issue"]) != UINT64_MAX
            or any(int(fields[name]) != 0 for name in (
                "active_count", "ready_queue", "pending_receive", "pending_transmit",
                "pending_dma", "active_receive_states", "reported_receive_states",
            ))
        ):
            fail(f"tile {tile} did not terminate quiescently")
    terminal_totals = {
        name: sum(int(fields[name]) for fields in terminal.values())
        for name in ("issued", "retired", "physical_global_ram_dma_submitted", "physical_global_ram_dma_completed")
    }
    expected_requests = sum(
        require_counter(compile_data["counters"], PHYSICAL_FIELDS[direction]["requests"], "compile")
        for direction in ("input", "output")
    )
    if terminal_totals["physical_global_ram_dma_submitted"] != expected_requests or terminal_totals["physical_global_ram_dma_completed"] != expected_requests:
        fail(f"SST DMA completion does not reconcile with compile evidence: {evidence}")

    stop_counts: dict[str, int] = {}
    for reason, count in STOP_PATTERN.findall(log):
        if reason in stop_counts:
            fail(f"duplicate stop-reason summary {reason}: {evidence}")
        stop_counts[reason] = int(count)
    if stop_counts.get("guest-exit") != len(tiles) or stop_counts.get("analog-submit", 0) <= 0:
        fail(f"SST guest/analog completion accounting is incomplete: {evidence}")

    router = router_summary(evidence / "router-statistics.csv")
    expected_bytes = sum(
        require_counter(compile_data["counters"], PHYSICAL_FIELDS[direction]["bytes"], "compile")
        for direction in ("input", "output")
    )
    if router["requests"] != expected_requests or router["bytes"] != expected_bytes:
        fail(f"SST router counters do not reconcile with compile evidence: {evidence}")
    return {
        "directory": str(evidence),
        "result_sha256": sha256(evidence / "result.csv"),
        "simulation_log_sha256": sha256(log_path),
        "launch_policy": normalized_launch(launch),
        "simulated_time": result["simulated_time"],
        "simulated_ns": simulated_ns,
        "wall_seconds": wall_seconds,
        "terminal_totals": terminal_totals,
        "stop_counts": stop_counts,
        "router": router,
    }


def normalized_launch(launch: dict[str, Any]) -> dict[str, Any]:
    """Return the simulator policy independent of append-only evidence paths.

    Reused SST runs copy the progress classifier and partition validator into
    each evidence directory.  Their absolute paths therefore differ across
    repetitions even though their recorded content digests are identical.
    Tool content is part of the policy; the directory chosen to preserve that
    content is not.
    """
    result = dict(launch)
    for name in (
        "source_compile_directory",
        "source_run_manifest_sha256",
        "materialization_audit_sha256",
        "physical_global_dma_work",
        "sst_work_partition",
    ):
        result.pop(name, None)
    runtime_tools = launch.get("runtime_tools")
    if runtime_tools is not None:
        if not isinstance(runtime_tools, dict) or not runtime_tools:
            fail("SST launch runtime_tools is malformed")
        normalized_tools: dict[str, dict[str, str]] = {}
        for name, record in runtime_tools.items():
            if not isinstance(name, str) or not name or not isinstance(record, dict):
                fail("SST launch runtime_tools is malformed")
            digest = record.get("sha256")
            resolved_path = record.get("resolved_path")
            if (
                not isinstance(digest, str)
                or re.fullmatch(r"[0-9a-f]{64}", digest) is None
                or not isinstance(resolved_path, str)
                or not resolved_path
            ):
                fail(f"SST launch runtime tool {name} is malformed")
            normalized_tools[name] = {"sha256": digest}
        result["runtime_tools"] = normalized_tools
    return result


def validate_sst_differential(
    control_paths: Iterable[Path], candidate_paths: Iterable[Path],
    control: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, Any]:
    control_runs = [load_sst(path, control) for path in control_paths]
    candidate_runs = [load_sst(path, candidate) for path in candidate_paths]
    if len(control_runs) != 3 or len(candidate_runs) != 3:
        fail("Phase 5 requires exactly three clean SST runs per arm")
    all_runs = control_runs + candidate_runs
    reference_policy = all_runs[0]["launch_policy"]
    if any(run["launch_policy"] != reference_policy for run in all_runs[1:]):
        fail("SST launch policy differs within the three-run A/B")

    for label, runs in (("control", control_runs), ("candidate", candidate_runs)):
        simulated = {run["simulated_ns"] for run in runs}
        analog = {run["stop_counts"]["analog-submit"] for run in runs}
        if len(simulated) != 1 or len(analog) != 1:
            fail(f"{label} repeated runs are not deterministic")
    control_analog = control_runs[0]["stop_counts"]["analog-submit"]
    candidate_analog = candidate_runs[0]["stop_counts"]["analog-submit"]
    if control_analog != candidate_analog:
        fail("candidate analog command completion count differs from control")
    control_ns = control_runs[0]["simulated_ns"]
    candidate_ns = candidate_runs[0]["simulated_ns"]
    if candidate_ns >= control_ns:
        fail("candidate modeled completion time is not strictly lower")
    control_wall = median(run["wall_seconds"] for run in control_runs)
    candidate_wall = median(run["wall_seconds"] for run in candidate_runs)
    if candidate_wall > control_wall:
        fail("candidate three-run wall median regressed without an approved explanation")
    return {
        "control": control_runs,
        "candidate": candidate_runs,
        "logical_completion": {
            "semantic_epoch_count": require_counter(control["counters"], "epoch_count", "control"),
            "active_tile_count": len(control["tiles"]),
            "analog_commands_completed": control_analog,
        },
        "modeled_time": {
            "control_ns": control_ns,
            "candidate_ns": candidate_ns,
            "reduction_percent": 100.0 * (control_ns - candidate_ns) / control_ns,
        },
        "wall_time": {
            "control_seconds": [run["wall_seconds"] for run in control_runs],
            "candidate_seconds": [run["wall_seconds"] for run in candidate_runs],
            "control_median_seconds": control_wall,
            "candidate_median_seconds": candidate_wall,
            "median_delta_percent": 100.0 * (candidate_wall - control_wall) / control_wall,
        },
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    control = load_compile(args.control_compile, False)
    candidate = load_compile(args.candidate_compile, True)
    frozen = compare_frozen_inputs(control, candidate)
    certificate = validate_scalar_certificate(candidate)
    compile_work = compare_compile_work(control, candidate, certificate)
    sst = validate_sst_differential(args.control_sst, args.candidate_sst, control, candidate)
    return {
        "schema": "golem.sculptor-fast-phase5-differential",
        "schema_version": 1,
        "status": "PASS",
        "interpretation": (
            "same-tree selected-region control versus scalar contraction; exact "
            "certificate deltas close against observed compile and SST physical work"
        ),
        "compile": {
            "frozen_inputs": frozen,
            "control_directory": control["directory"],
            "candidate_directory": candidate["directory"],
            "scalar_certificate": certificate,
            "work": compile_work,
        },
        "sst": sst,
        "errors": [],
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    if path.is_symlink():
        fail(f"report path is symbolic: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-compile", type=Path, required=True)
    parser.add_argument("--candidate-compile", type=Path, required=True)
    parser.add_argument("--control-sst", type=Path, action="append", required=True)
    parser.add_argument("--candidate-sst", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = build_report(args)
        write_report(args.output, report)
    except (DifferentialError, OSError, UnicodeError) as error:
        print(f"FAST Phase-5 differential: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "FAST Phase-5 differential: PASS "
        f"regions={report['compile']['scalar_certificate']['contracted_region_count']} "
        f"simulated_reduction={report['sst']['modeled_time']['reduction_percent']:.2f}% "
        f"wall_median_delta={report['sst']['wall_time']['median_delta_percent']:+.2f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
