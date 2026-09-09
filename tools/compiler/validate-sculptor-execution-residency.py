#!/usr/bin/env python3
"""Validate the hash-bound Sculptor execution-residency audit.

Phase 0 deliberately changes no compiler semantics. Phase 2 discovers
candidates but still changes no physical behavior. This validator checks the
audit contract, the analysis-only metadata boundary, and—when complete compile
runs are supplied—the physical deployment, objects, ELFs, and accounting.
Later FAST phases extend this same entry point with mapping, materialization,
ABI, and SST reconciliations.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys
from typing import Any


SCHEMA = "sculptor.execution-residency-audit"
SCHEMA_VERSION = 1
PLAN_VERSION = 1
BUFFER_BYTES = 1024 * 1024
ZERO_CHANGE_FIELDS = (
    "physical_change_count",
    "candidate_count",
    "selected_region_count",
    "selected_member_count",
    "selected_internal_edge_count",
    "analog_digital_region_count",
    "digital_only_region_count",
    "candidate_logical_bytes",
    "estimated_avoided_input_bytes",
    "estimated_avoided_output_bytes",
    "estimated_avoided_requests",
    "actual_elided_input_descriptors",
    "actual_elided_output_descriptors",
    "actual_elided_input_bytes",
    "actual_elided_output_bytes",
    "actual_elided_physical_requests",
    "preserved_external_spills",
    "preserved_remote_routes",
    "region_ring_bytes",
    "region_temporary_bytes",
    "scalar_region_invocations",
    "wave_region_invocations",
    "region_static_instructions",
    "region_dynamic_instructions",
    "region_dispatches",
    "analog_commands_submitted",
    "analog_commands_completed",
    "logical_internal_bytes",
    "physical_internal_bytes",
)
PHYSICAL_ZERO_FIELDS = (
    "physical_change_count",
    "selected_region_count",
    "selected_member_count",
    "selected_internal_edge_count",
    "analog_digital_region_count",
    "digital_only_region_count",
    "actual_elided_input_descriptors",
    "actual_elided_output_descriptors",
    "actual_elided_input_bytes",
    "actual_elided_output_bytes",
    "actual_elided_physical_requests",
    "preserved_external_spills",
    "preserved_remote_routes",
    "region_ring_bytes",
    "region_temporary_bytes",
    "scalar_region_invocations",
    "wave_region_invocations",
    "region_static_instructions",
    "region_dynamic_instructions",
    "region_dispatches",
    "analog_commands_submitted",
    "analog_commands_completed",
    "logical_internal_bytes",
    "physical_internal_bytes",
)
SELECT_PREMATERIALIZATION_ZERO_FIELDS = (
    "physical_change_count",
    "actual_elided_input_descriptors",
    "actual_elided_output_descriptors",
    "actual_elided_input_bytes",
    "actual_elided_output_bytes",
    "actual_elided_physical_requests",
    "preserved_external_spills",
    "preserved_remote_routes",
    "region_ring_bytes",
    "region_temporary_bytes",
    "scalar_region_invocations",
    "wave_region_invocations",
    "region_static_instructions",
    "region_dynamic_instructions",
    "region_dispatches",
    "analog_commands_submitted",
    "analog_commands_completed",
    "physical_internal_bytes",
)
EMPTY_OBJECT_FIELDS = (
    "candidate_rejections_by_reason",
    "per_tile_peak_spm_bytes",
    "per_tile_headroom_bytes",
    "wave_width_histogram",
)
EMPTY_ARRAY_FIELDS = ("regions", "tiles")
CORE_ARTIFACT_SUFFIXES = (
    "-extracted.mlir",
    "-runtime-graph.mlir",
    "-planned.mlir",
    "-finalized.mlir",
    "-compute-llvm.mlir",
    "-task-only.mlir",
    ".ll",
    ".o",
)
DEPLOYMENT_ARTIFACTS = (
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
    "placement-summary.csv",
)
ANALYSIS_METADATA_DEPLOYMENT_ARTIFACTS = frozenset(
    (
        "04-residency-regions.mlir",
        "05-ra-tree.mlir",
        "06-mapping-plan.mlir",
        "08-placed.mlir",
    )
)
ROOT_EQUIVALENT_ARTIFACTS = (
    "model.mlir",
    "model.expected.json",
    "active-cores.txt",
    "deployment-manifest.json",
    "abi-preflight-summary.txt",
    "idle.elf",
)
NORMALIZED_JSON_ARTIFACTS = (
    "streaming-architecture.json",
    "materialization-audit.json",
    "memory-reports/tile-memory-summary.json",
)
EVIDENCE_FILES = (
    "launch.json",
    "partition.txt",
    "partition-summary.json",
    "progress-classification.json",
    "resource-usage.txt",
    "result.csv",
    "router-statistics.csv",
    "simulation.log",
    "status.csv",
)
STOP_REASON_PATTERN = re.compile(
    r"MITTENS_QEMU_CAPTURE_HOST stop_reason=([^\s(]+)\([0-9]+\) "
    r"count=([0-9]+) total_ns=[0-9]+ max_ns=[0-9]+"
)
RESIDENCY_MODE_PATTERN = re.compile(
    rb'(?P<name>sculptor\.execution_residency\.mode|executionResidencyMode)'
    rb' = "(?P<mode>off|analyze)"'
)


class ValidationError(RuntimeError):
    """The audit is missing, stale, malformed, or physically non-neutral."""


def fail(message: str) -> None:
    raise ValidationError(message)


def normalize_mode(value: str) -> str:
    aliases = {"0": "off", "off": "off", "analyze": "analyze",
               "1": "select", "select": "select"}
    try:
        return aliases[value]
    except KeyError:
        fail(f"invalid execution-residency mode: {value}")


def parse_bool(value: str) -> bool:
    if value in {"1", "true"}:
        return True
    if value in {"0", "false"}:
        return False
    raise argparse.ArgumentTypeError("expected true, false, 1, or 0")


def require_regular_file(path: Path, label: str) -> None:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        fail(f"{label} is missing, empty, or symbolic: {path}")


def digest_file(path: Path) -> tuple[str, int]:
    require_regular_file(path, "artifact")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(BUFFER_BYTES):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def read_json(path: Path, label: str) -> dict[str, Any]:
    require_regular_file(path, label)
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {label}: {error}")
    if not isinstance(payload, dict):
        fail(f"{label} must contain one JSON object")
    return payload


def read_csv_row(path: Path, label: str) -> dict[str, str]:
    require_regular_file(path, label)
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        rows = list(reader)
    if reader.fieldnames is None or len(rows) != 1:
        fail(f"{label} must contain one header and one data row")
    if None in rows[0]:
        fail(f"{label} contains columns beyond its header")
    return rows[0]


def canonical_json(value: Any, run_directory: Path) -> Any:
    """Replace only run-local provenance paths, preserving semantic fields."""
    if isinstance(value, dict):
        return {
            key: canonical_json(child, run_directory)
            for key, child in sorted(value.items())
        }
    if isinstance(value, list):
        return [canonical_json(child, run_directory) for child in value]
    if isinstance(value, str):
        return value.replace(str(run_directory.resolve()), "${COMPILE_RUN}")
    return value


def require_equal_files(control: Path, candidate: Path, label: str) -> str:
    control_sha256, control_bytes = digest_file(control)
    candidate_sha256, candidate_bytes = digest_file(candidate)
    if control_bytes != candidate_bytes or control_sha256 != candidate_sha256:
        fail(f"Phase-0 analyze differs from off control: {label}")
    return control_sha256


def require_equal_except_analysis_mode(
    control: Path, candidate: Path, label: str
) -> str:
    """Require exact equality after normalizing only typed analysis mode.

    Phase-2 analysis deliberately carries an empty typed plan with mode
    ``analyze`` through the RA tree. The off control carries the same empty
    plan with mode ``off``. No other byte—including graph fingerprints,
    region/member/edge tables, mapping, or placement—may differ.
    """

    def normalize(path: Path, expected_mode: str) -> tuple[bytes, int]:
        require_regular_file(path, label)
        payload = path.read_bytes()
        matches = list(RESIDENCY_MODE_PATTERN.finditer(payload))
        if not matches:
            fail(f"analysis-mode metadata is missing: {label}")
        modes = {match.group("mode").decode("ascii") for match in matches}
        if modes != {expected_mode}:
            fail(f"analysis-mode metadata is stale or mixed: {label}")
        normalized = RESIDENCY_MODE_PATTERN.sub(
            lambda match: match.group("name") + b' = "analysis-only"', payload
        )
        return normalized, len(matches)

    control_payload, control_count = normalize(control, "off")
    candidate_payload, candidate_count = normalize(candidate, "analyze")
    if control_count != candidate_count or control_payload != candidate_payload:
        fail(
            "Phase-2 analysis differs from off control beyond typed mode "
            f"metadata: {label}"
        )
    return hashlib.sha256(control_payload).hexdigest()


def artifact_set(directory: Path, suffix: str) -> dict[int, Path]:
    if not directory.is_dir() or directory.is_symlink():
        fail(f"artifact directory is missing or symbolic: {directory}")
    escaped = re.escape(suffix)
    pattern = re.compile(rf"core-([0-9]+){escaped}$")
    result: dict[int, Path] = {}
    for path in directory.iterdir():
        match = pattern.fullmatch(path.name)
        if match is None:
            continue
        tile = int(match.group(1))
        if tile in result:
            fail(f"duplicate core artifact for tile {tile}: {suffix}")
        result[tile] = path
    return result


def tile_elf_set(directory: Path) -> dict[int, Path]:
    pattern = re.compile(r"tile-([0-9]+)\.elf$")
    result: dict[int, Path] = {}
    for path in directory.glob("tile-*.elf"):
        match = pattern.fullmatch(path.name)
        if match is not None:
            result[int(match.group(1))] = path
    return result


def fingerprint_entries(entries: list[tuple[str, str]]) -> str:
    digest = hashlib.sha256()
    for name, artifact_sha256 in sorted(entries):
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(artifact_sha256.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def first_difference(control: Any, candidate: Any, path: str = "identity") -> str:
    if type(control) is not type(candidate):
        return f"{path} type {type(control).__name__} != {type(candidate).__name__}"
    if isinstance(control, dict):
        if set(control) != set(candidate):
            missing = sorted(set(control) - set(candidate))
            extra = sorted(set(candidate) - set(control))
            return f"{path} keys missing={missing} extra={extra}"
        for key in sorted(control):
            if control[key] != candidate[key]:
                return first_difference(control[key], candidate[key], f"{path}.{key}")
    elif isinstance(control, list):
        if len(control) != len(candidate):
            return f"{path} length {len(control)} != {len(candidate)}"
        for index, (left, right) in enumerate(zip(control, candidate)):
            if left != right:
                return first_difference(left, right, f"{path}[{index}]")
    elif control != candidate:
        return f"{path} {control!r} != {candidate!r}"
    return f"{path} differs"


def parse_resource_usage(path: Path) -> dict[str, int | float]:
    require_regular_file(path, "SST resource usage")
    fields: dict[str, int | float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            fail(f"malformed SST resource usage line: {line}")
        key, raw_value = line.split("=", 1)
        raw_value = raw_value.removesuffix("%")
        try:
            fields[key] = float(raw_value) if "." in raw_value else int(raw_value)
        except ValueError:
            fail(f"non-numeric SST resource usage field: {key}")
    for required in ("sst_wall_seconds", "sst_max_rss_kib"):
        if required not in fields:
            fail(f"SST resource usage lacks {required}")
    return fields


def parse_stop_counts(log_text: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for reason, raw_count in STOP_REASON_PATTERN.findall(log_text):
        if reason in counts:
            fail(f"duplicate QEMU stop-reason summary: {reason}")
        counts[reason] = int(raw_count)
    if not counts:
        fail("completed SST log lacks QEMU stop-reason summaries")
    return counts


def router_statistics_rows(path: Path) -> tuple[list[str], list[tuple[str, ...]]]:
    require_regular_file(path, "SST router statistics")
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.reader(source)
        try:
            header = next(reader)
        except StopIteration:
            fail("SST router statistics are empty")
        rows = [tuple(row) for row in reader]
    if not header or not rows or any(len(row) != len(header) for row in rows):
        fail("SST router statistics are malformed")
    return header, sorted(rows)


def summarize_statistics(
    header: list[str], rows: list[tuple[str, ...]]
) -> dict[str, int]:
    index = {name: offset for offset, name in enumerate(header)}
    for field in ("ComponentName", "StatisticName", "Sum.u64"):
        if field not in index:
            fail(f"SST router statistics lack {field}")
    summary: dict[str, int] = {}
    selected = {
        ("global_ram", "requests"),
        ("global_ram", "bytes"),
        ("global_ram", "readiness_delay_cycles"),
        ("global_ram", "queue_delay_cycles"),
        ("global_ram", "service_cycles"),
        ("global_ram", "execution_teardown_wait_cycles"),
        ("global_ram", "maximum_queue_occupancy"),
        ("global_ram", "readiness_blocked_reads"),
        ("global_ram", "readiness_publications"),
        ("global_ram", "readiness_releases"),
    }
    for row in rows:
        key = (row[index["ComponentName"]], row[index["StatisticName"]])
        if key in selected:
            try:
                summary[key[1]] = int(row[index["Sum.u64"]])
            except ValueError:
                fail(f"non-integer SST statistic: {key[0]}.{key[1]}")
    if set(summary) != {name for _, name in selected}:
        fail("SST router statistics lack required global-RAM counters")
    return summary


def require_nonnegative_int(payload: dict[str, Any], field: str) -> int:
    value = payload.get(field)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        fail(f"{field} must be a nonnegative integer")
    return value


def validate_analysis_candidates(
    payload: dict[str, Any], expected_mode: str
) -> None:
    candidates = payload.get("regions")
    if not isinstance(candidates, list):
        fail("Phase-2 regions must contain the compact candidate table")
    candidate_count = require_nonnegative_int(payload, "candidate_count")
    if candidate_count != len(candidates):
        fail("Phase-2 candidate_count disagrees with the candidate table")
    recommended_count = require_nonnegative_int(
        payload, "recommended_candidate_count"
    )
    analog_digital_count = require_nonnegative_int(
        payload, "candidate_analog_digital_count"
    )
    if recommended_count > candidate_count or analog_digital_count > recommended_count:
        fail("Phase-2 recommended candidate accounting is inconsistent")

    expected_rejections: dict[str, int] = {}
    expected_growth_stops: dict[str, int] = {}
    occupied_work_units: set[int] = set()
    aggregate_logical_bytes = 0
    aggregate_input_bytes = 0
    aggregate_output_bytes = 0
    aggregate_requests = 0
    observed_recommended = 0
    observed_analog_digital = 0
    valid_roles = {
        "view", "digital_pre", "analog_mvm", "local_reduction", "digital_post"
    }
    benefit_fields = {
        "avoided_ram_setup_cycles",
        "avoided_ram_transfer_cycles",
        "predicted_avoided_ram_queue_cycles",
        "avoided_spm_endpoint_cycles",
        "avoided_noc_cycles",
        "avoided_runtime_dispatch_cycles",
        "avoided_dependency_transition_cycles",
        "exposed_analog_digital_overlap_cycles",
    }
    cost_fields = {
        "added_digital_lane_contention",
        "added_analog_lane_contention",
        "lost_inter_tile_parallelism",
        "added_local_assembly_or_reduction",
        "added_spm_bank_or_port_contention",
        "code_and_dispatch_cost",
        "pipeline_fill_and_drain",
    }

    for ordinal, candidate in enumerate(candidates):
        if not isinstance(candidate, dict) or candidate.get("candidate_ordinal") != ordinal:
            fail("Phase-2 candidate ordinals must be dense and canonical")
        for field in (
            "anchor_work_unit_id", "anchor_operation_id", "iteration_begin",
            "iteration_end", "iteration_step", "member_count",
            "internal_edge_count", "estimated_spm_bytes_per_slot",
            "estimated_avoided_input_bytes", "estimated_avoided_output_bytes",
            "estimated_avoided_requests", "logical_internal_bytes",
            "saved_physical_bytes", "saved_critical_cycle_lower_bound",
        ):
            require_nonnegative_int(candidate, field)
        if candidate["iteration_end"] <= candidate["iteration_begin"]:
            fail("Phase-2 candidate domain is empty")
        if candidate["iteration_step"] <= 0:
            fail("Phase-2 candidate iteration step is not positive")

        members = candidate.get("members")
        edges = candidate.get("internal_edge_ordinals")
        rejections = candidate.get("rejections")
        growth_stops = candidate.get("growth_stops")
        if (
            not isinstance(members, list)
            or not isinstance(edges, list)
            or not isinstance(rejections, list)
            or not isinstance(growth_stops, list)
            or candidate["member_count"] != len(members)
            or candidate["internal_edge_count"] != len(edges)
            or edges != sorted(set(edges))
        ):
            fail("Phase-2 candidate compact tables are malformed")
        member_ids: list[int] = []
        roles: set[str] = set()
        for stage, member in enumerate(members):
            if not isinstance(member, dict) or member.get("stage_ordinal") != stage:
                fail("Phase-2 candidate member stages are not canonical")
            work_unit_id = require_nonnegative_int(member, "work_unit_id")
            require_nonnegative_int(member, "operation_id")
            role = member.get("role")
            if role not in valid_roles:
                fail("Phase-2 candidate contains an unknown semantic role")
            member_ids.append(work_unit_id)
            roles.add(role)
        if len(member_ids) != len(set(member_ids)):
            fail("Phase-2 candidate repeats a work unit")
        if any(not isinstance(reason, str) or not reason for reason in rejections):
            fail("Phase-2 candidate rejection reasons are not typed strings")
        if any(not isinstance(reason, str) or not reason for reason in growth_stops):
            fail("Phase-2 candidate growth-stop reasons are not typed strings")
        for reason in set(rejections):
            expected_rejections[reason] = expected_rejections.get(reason, 0) + 1
        for reason in set(growth_stops):
            expected_growth_stops[reason] = expected_growth_stops.get(reason, 0) + 1

        recommended = candidate.get("recommended")
        expected_disposition = (
            "selected"
            if expected_mode == "select" and recommended is True
            else "recommended"
            if recommended is True
            else "rejected"
        )
        if not isinstance(recommended, bool) or candidate.get("disposition") != expected_disposition:
            fail("Phase-2 candidate disposition is inconsistent")
        selected = candidate.get("selected")
        if expected_mode == "select":
            if not isinstance(selected, bool) or selected != recommended:
                fail("Phase-3 selected candidate disposition is inconsistent")
        elif selected is not None:
            fail("analysis candidate unexpectedly carries selected state")
        if recommended:
            if rejections:
                fail("Phase-2 recommended candidate carries a rejection")
            if "analog_mvm" not in roles or not roles.intersection(
                {"digital_pre", "local_reduction", "digital_post"}
            ):
                fail("Phase-2 recommended candidate is not analog-digital")
            if occupied_work_units.intersection(member_ids):
                fail("Phase-2 overlap resolution selected a work unit twice")
            occupied_work_units.update(member_ids)
            observed_recommended += 1
            observed_analog_digital += 1
            aggregate_logical_bytes += candidate["logical_internal_bytes"]
            aggregate_input_bytes += candidate["estimated_avoided_input_bytes"]
            aggregate_output_bytes += candidate["estimated_avoided_output_bytes"]
            aggregate_requests += candidate["estimated_avoided_requests"]

        cycle_model = candidate.get("cycle_model")
        if (
            not isinstance(cycle_model, dict)
            or cycle_model.get("status")
            != "deferred_until_mapping_hardware_profile"
            or cycle_model.get("complete") is not False
            or set(cycle_model.get("benefit_cycle_lower_bounds", {}))
            != benefit_fields
            or set(cycle_model.get("cost_cycle_lower_bounds", {})) != cost_fields
            or any(
                not isinstance(value, int) or isinstance(value, bool) or value < 0
                for table in (
                    cycle_model["benefit_cycle_lower_bounds"],
                    cycle_model["cost_cycle_lower_bounds"],
                )
                for value in table.values()
            )
        ):
            fail("Phase-2 candidate cycle-cost component table is incomplete")

    if observed_recommended != recommended_count:
        fail("Phase-2 recommended candidate count does not reconcile")
    if observed_analog_digital != analog_digital_count:
        fail("Phase-2 analog-digital candidate count does not reconcile")
    if expected_mode == "select":
        if require_nonnegative_int(payload, "selected_region_count") != observed_recommended:
            fail("Phase-3 selected region count does not reconcile")
        expected_members = sum(
            candidate["member_count"]
            for candidate in candidates
            if candidate["recommended"]
        )
        expected_edges = sum(
            candidate["internal_edge_count"]
            for candidate in candidates
            if candidate["recommended"]
        )
        if require_nonnegative_int(payload, "selected_member_count") != expected_members:
            fail("Phase-3 selected member count does not reconcile")
        if require_nonnegative_int(payload, "selected_internal_edge_count") != expected_edges:
            fail("Phase-3 selected edge count does not reconcile")
    if payload.get("candidate_rejections_by_reason") != expected_rejections:
        fail("Phase-2 candidate rejection counts do not reconcile")
    if payload.get("candidate_growth_stops_by_reason") != expected_growth_stops:
        fail("Phase-2 candidate growth-stop counts do not reconcile")
    expected_aggregates = {
        "candidate_logical_bytes": aggregate_logical_bytes,
        "estimated_avoided_input_bytes": aggregate_input_bytes,
        "estimated_avoided_output_bytes": aggregate_output_bytes,
        "estimated_avoided_requests": aggregate_requests,
    }
    for field, expected in expected_aggregates.items():
        if require_nonnegative_int(payload, field) != expected:
            fail(f"Phase-2 candidate aggregate does not reconcile: {field}")


def validate(args: argparse.Namespace) -> dict[str, Any]:
    require_regular_file(args.audit, "execution-residency audit")
    try:
        payload = json.loads(args.audit.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read execution-residency audit: {error}")
    if not isinstance(payload, dict):
        fail("execution-residency audit must contain one JSON object")
    if payload.get("schema") != SCHEMA or payload.get("schema_version") != SCHEMA_VERSION:
        fail("execution-residency audit schema is invalid")
    if payload.get("status") != "PASS":
        fail("execution-residency audit is not a PASS certificate")
    expected_mode = normalize_mode(args.expected_mode)
    phase = payload.get("phase")
    allowed_phases = (
        {0}
        if expected_mode == "off"
        else {0, 2}
        if expected_mode == "analyze"
        else {3}
    )
    if phase not in allowed_phases or payload.get("plan_version") != PLAN_VERSION:
        fail("execution-residency audit has an invalid phase or plan version")
    if payload.get("mode") != expected_mode:
        fail("execution-residency audit mode is stale or mismatched")
    expected_configuration = {
        "maximum_members": args.expected_maximum_members,
        "maximum_wave_width": args.expected_maximum_wave_width,
        "require_positive_benefit": args.expected_require_positive_benefit,
    }
    if payload.get("configuration") != expected_configuration:
        fail("execution-residency audit configuration is stale or mismatched")
    description = (
        f"mode={expected_mode}\n"
        f"maximum_members={args.expected_maximum_members}\n"
        f"maximum_wave_width={args.expected_maximum_wave_width}\n"
        "require_positive_benefit="
        f"{'true' if args.expected_require_positive_benefit else 'false'}\n"
    ).encode("utf-8")
    expected_configuration_hash = hashlib.sha256(description).hexdigest()
    if payload.get("configuration_sha256") != expected_configuration_hash:
        fail("execution-residency configuration fingerprint is invalid")

    graph_fingerprint = payload.get("graph_fingerprint")
    source_fingerprint = payload.get("source_ir_sha256")
    for name, value in (
        ("graph_fingerprint", graph_fingerprint),
        ("source_ir_sha256", source_fingerprint),
    ):
        if (
            not isinstance(value, str)
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
        ):
            fail(f"{name} is not a SHA-256 fingerprint")
    if require_nonnegative_int(payload, "canonical_ir_bytes") == 0:
        fail("canonical_ir_bytes must be positive")
    if require_nonnegative_int(payload, "source_ir_bytes") == 0:
        fail("source_ir_bytes must be positive")

    if payload.get("semantic_ir_changed") is not False:
        fail("execution-residency planning reports an IR semantic change")
    if payload.get("accounting_complete") is not False:
        fail("analysis-only audit must not claim complete physical accounting")
    zero_fields = (
        ZERO_CHANGE_FIELDS
        if phase == 0
        else PHYSICAL_ZERO_FIELDS
        if phase == 2
        else SELECT_PREMATERIALIZATION_ZERO_FIELDS
    )
    for field in zero_fields:
        if require_nonnegative_int(payload, field) != 0:
            fail(f"analysis-only mode reports nonzero physical change: {field}")
    if phase == 0:
        for field in EMPTY_OBJECT_FIELDS:
            if payload.get(field) != {}:
                fail(f"Phase-0 analyze/off mode requires an empty {field}")
        for field in EMPTY_ARRAY_FIELDS:
            if payload.get(field) != []:
                fail(f"Phase-0 analyze/off mode requires an empty {field}")
        if payload.get("recommended_candidate_count", 0) != 0:
            fail("Phase-0 audit reports recommended candidates")
        if payload.get("candidate_analog_digital_count", 0) != 0:
            fail("Phase-0 audit reports analog-digital candidates")
        if payload.get("candidate_growth_stops_by_reason", {}) != {}:
            fail("Phase-0 audit reports candidate growth stops")
    else:
        validate_analysis_candidates(payload, expected_mode)
        if payload.get("tiles") != []:
            fail("Phase-2 analysis cannot contain physical tile records")

    if args.control_mlir is not None and args.output_mlir is None:
        fail("--control-mlir requires --output-mlir")
    differential_sha256 = None
    if args.input_mlir is not None:
        input_sha256, input_bytes = digest_file(args.input_mlir)
        if input_sha256 != source_fingerprint or input_bytes != payload["source_ir_bytes"]:
            fail("execution-residency audit is stale relative to its source MLIR")
    if args.output_mlir is not None:
        output_sha256, _ = digest_file(args.output_mlir)
        if output_sha256 != graph_fingerprint:
            fail("execution-residency audit graph fingerprint disagrees with output MLIR")
        if args.control_mlir is not None:
            control_sha256, control_bytes = digest_file(args.control_mlir)
            output_bytes = args.output_mlir.stat().st_size
            if control_bytes != output_bytes or control_sha256 != output_sha256:
                fail("Phase-0 analyze mode differs from the off/control MLIR")
            differential_sha256 = output_sha256

    return {
        "schema": "sculptor.execution-residency-validation",
        "schema_version": 1,
        "status": "PASS",
        "mode": expected_mode,
        "phase": phase,
        "configuration_sha256": expected_configuration_hash,
        "source_ir_sha256": source_fingerprint,
        "differential_sha256": differential_sha256,
        "physical_change_count": payload["physical_change_count"],
        "candidate_count": payload["candidate_count"],
        "recommended_candidate_count": payload.get(
            "recommended_candidate_count", 0
        ),
        "candidate_logical_bytes": payload["candidate_logical_bytes"],
        "estimated_avoided_input_bytes": payload[
            "estimated_avoided_input_bytes"
        ],
        "estimated_avoided_output_bytes": payload[
            "estimated_avoided_output_bytes"
        ],
        "estimated_avoided_requests": payload["estimated_avoided_requests"],
    }


def validate_compile_run(
    run: Path,
    expected_mode: str,
    args: argparse.Namespace,
) -> tuple[list[int], dict[str, Any]]:
    if run.is_symlink() or not run.is_dir():
        fail(f"compile run is missing or symbolic: {run}")
    qualification = read_json(run / "compile-qualification.json", "qualification")
    if (
        qualification.get("schema") != "sculptor.compile-qualification"
        or qualification.get("status") != "PASS"
    ):
        fail(f"compile qualification did not pass: {run}")
    checks = qualification.get("checks")
    required_checks = {
        "abi_preflight",
        "complete_lowering",
        "elf_generation",
        "execution_residency_audit",
        "materialization_audit",
        "memory_validation",
        "optimization_contracts",
    }
    if not isinstance(checks, dict) or not required_checks.issubset(checks):
        fail(f"compile qualification is missing required checks: {run}")
    for name in required_checks:
        check = checks[name]
        if not isinstance(check, dict) or check.get("status") != "PASS":
            fail(f"compile qualification check did not pass: {name}")
    audit_check = checks["execution_residency_audit"]
    allowed_phases = {0} if expected_mode == "off" else {0, 2}
    if (
        audit_check.get("mode") != expected_mode
        or audit_check.get("phase") not in allowed_phases
    ):
        fail(f"compile qualification carries a stale residency mode: {run}")

    deployment = read_json(run / "deployment-manifest.json", "deployment manifest")
    active_tiles = deployment.get("active_tile_ids")
    if (
        deployment.get("schema") != "sculptor.deployment"
        or not isinstance(active_tiles, list)
        or not active_tiles
        or any(isinstance(tile, bool) or not isinstance(tile, int) for tile in active_tiles)
        or active_tiles != sorted(set(active_tiles))
    ):
        fail(f"deployment manifest has an invalid active-tile set: {run}")

    architecture = read_json(run / "streaming-architecture.json", "architecture")
    streaming = architecture.get("architecture")
    expected_streaming = {
        "fixed_shard_bytes": 4096,
        "global_ram_bytes": 32 * 1024 * 1024 * 1024,
        "max_in_flight": 2,
        "noc_word_bytes": 4,
        "scratchpad_bytes": 2 * 1024 * 1024,
        "version": 1,
    }
    if streaming != expected_streaming:
        fail(f"Phase-0 streaming architecture is not frozen: {run}")

    materialization = read_json(
        run / "materialization-audit.json", "materialization audit"
    )
    if (
        materialization.get("status") != "PASS"
        or materialization.get("active_tile_ids") != active_tiles
        or materialization.get("maximum_frame_bytes") != 4096
    ):
        fail(f"materialization audit is incomplete or mismatched: {run}")
    memory = read_json(
        run / "memory-reports" / "tile-memory-summary.json", "memory audit"
    )
    if (
        memory.get("active_tile_count") != len(active_tiles)
        or memory.get("scratchpad_capacity_bytes") != 2 * 1024 * 1024
        or not isinstance(memory.get("capacity_gate"), dict)
        or memory["capacity_gate"].get("status") != "PASS"
        or memory["capacity_gate"].get("errors") != []
    ):
        fail(f"tile-memory audit is incomplete or over capacity: {run}")

    audit = run / "execution-residency-audit.json"
    audit_sha256, audit_bytes = digest_file(audit)
    manifest = read_json(run / "run-manifest.json", "run manifest")
    artifacts = manifest.get("artifacts")
    bound_audit = artifacts.get("execution_residency_audit") if isinstance(
        artifacts, dict
    ) else None
    if (
        not isinstance(bound_audit, dict)
        or bound_audit.get("sha256") != audit_sha256
        or bound_audit.get("bytes") != audit_bytes
        or audit_check.get("audit_sha256") != audit_sha256
    ):
        fail(f"execution-residency audit is not hash-bound into the run: {run}")

    audit_args = argparse.Namespace(
        audit=audit,
        input_mlir=run / "deployment" / "04-tensor-fragments.mlir",
        control_mlir=None,
        output_mlir=run / "deployment" / "04-residency-regions.mlir",
        expected_mode=expected_mode,
        expected_maximum_members=args.expected_maximum_members,
        expected_maximum_wave_width=args.expected_maximum_wave_width,
        expected_require_positive_benefit=args.expected_require_positive_benefit,
    )
    audit_validation = validate(audit_args)
    if audit_validation["phase"] != audit_check["phase"]:
        fail(f"compile qualification carries a stale residency phase: {run}")
    return active_tiles, {
        "audit_sha256": audit_sha256,
        "audit_validation": audit_validation,
        "compile_qualification": qualification,
        "materialization": materialization,
        "memory": memory,
    }


def compare_compile_runs(
    control: Path,
    candidate: Path,
    args: argparse.Namespace,
) -> tuple[list[int], dict[str, Any]]:
    control_tiles, control_data = validate_compile_run(control, "off", args)
    candidate_tiles, candidate_data = validate_compile_run(candidate, "analyze", args)
    if candidate_tiles != control_tiles:
        fail("analysis active-tile set differs from off control")

    control_audit = control_data["audit_validation"]
    candidate_audit = candidate_data["audit_validation"]
    for field in ("source_ir_sha256", "physical_change_count"):
        if control_audit[field] != candidate_audit[field]:
            fail(f"analysis audit differential disagrees on {field}")
    candidate_phase = candidate_audit["phase"]

    entries: list[tuple[str, str]] = []
    for relative in ROOT_EQUIVALENT_ARTIFACTS:
        sha256 = require_equal_files(control / relative, candidate / relative, relative)
        entries.append((relative, sha256))
    for relative in DEPLOYMENT_ARTIFACTS:
        control_path = control / "deployment" / relative
        candidate_path = candidate / "deployment" / relative
        if relative == "03-duplicate-matrices.mlir" and (
            not control_path.exists() or not candidate_path.exists()
        ):
            if control_path.exists() != candidate_path.exists():
                fail("optional duplicate-matrices stage differs across analysis A/B")
            continue
        label = f"deployment/{relative}"
        if (
            candidate_phase == 2
            and relative in ANALYSIS_METADATA_DEPLOYMENT_ARTIFACTS
        ):
            sha256 = require_equal_except_analysis_mode(
                control_path, candidate_path, label
            )
        else:
            sha256 = require_equal_files(control_path, candidate_path, label)
        entries.append((f"deployment/{relative}", sha256))
    for relative in NORMALIZED_JSON_ARTIFACTS:
        control_payload = canonical_json(
            read_json(control / relative, relative), control
        )
        candidate_payload = canonical_json(
            read_json(candidate / relative, relative), candidate
        )
        if control_payload != candidate_payload:
            fail(f"normalized physical accounting differs: {relative}")
        encoded = json.dumps(
            control_payload, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        entries.append((relative, hashlib.sha256(encoded).hexdigest()))

    expected_set = set(control_tiles)
    for suffix in CORE_ARTIFACT_SUFFIXES:
        control_artifacts = artifact_set(control / "cores", suffix)
        candidate_artifacts = artifact_set(candidate / "cores", suffix)
        if set(control_artifacts) != expected_set or set(candidate_artifacts) != expected_set:
            fail(f"core artifact set is incomplete for suffix {suffix}")
        for tile in control_tiles:
            name = f"cores/core-{tile}{suffix}"
            sha256 = require_equal_files(
                control_artifacts[tile], candidate_artifacts[tile], name
            )
            entries.append((name, sha256))

    control_elfs = tile_elf_set(control)
    candidate_elfs = tile_elf_set(candidate)
    if set(control_elfs) != expected_set or set(candidate_elfs) != expected_set:
        fail("production tile ELF set differs from the active-tile set")
    for tile in control_tiles:
        name = f"tile-{tile}.elf"
        sha256 = require_equal_files(control_elfs[tile], candidate_elfs[tile], name)
        entries.append((name, sha256))

    task_count = 0
    routine_count = 0
    for tile in control_tiles:
        text = (control / "cores" / f"core-{tile}-finalized.mlir").read_text(
            encoding="utf-8"
        )
        task_count += len(re.findall(r"\bsculptor\.task\.create\b", text))
        routine_count += len(
            re.findall(
                r"^\s*func\.func private .*sculptor\.deployment\.global_routine_id",
                text,
                re.MULTILINE,
            )
        )
    if task_count <= 0 or routine_count <= 0 or task_count != routine_count:
        fail("static task/routine accounting is missing or inconsistent")

    counters = control_data["materialization"].get("counters")
    if not isinstance(counters, dict):
        fail("materialization audit lacks counters")
    memory_summaries = control_data["memory"].get("summaries")
    capacity = memory_summaries.get("capacity") if isinstance(
        memory_summaries, dict
    ) else None
    capacity_maximums = capacity.get("maximums") if isinstance(capacity, dict) else None
    if not isinstance(capacity_maximums, dict):
        fail("tile-memory audit lacks capacity maximums")
    maximum_required_spm = capacity_maximums.get("requiredLocalBytes")
    if (
        isinstance(maximum_required_spm, bool)
        or not isinstance(maximum_required_spm, int)
        or not 0 <= maximum_required_spm <= 2 * 1024 * 1024
    ):
        fail("tile-memory audit has an invalid maximum SPM requirement")
    return control_tiles, {
        "artifact_count": len(entries),
        "artifact_set_sha256": fingerprint_entries(entries),
        "control_phase": control_audit["phase"],
        "candidate_phase": candidate_phase,
        "candidate_count": candidate_audit["candidate_count"],
        "recommended_candidate_count": candidate_audit[
            "recommended_candidate_count"
        ],
        "candidate_logical_bytes": candidate_audit["candidate_logical_bytes"],
        "estimated_avoided_input_bytes": candidate_audit[
            "estimated_avoided_input_bytes"
        ],
        "estimated_avoided_output_bytes": candidate_audit[
            "estimated_avoided_output_bytes"
        ],
        "estimated_avoided_requests": candidate_audit[
            "estimated_avoided_requests"
        ],
        "active_tile_count": len(control_tiles),
        "semantic_epoch_count": counters.get("epoch_count"),
        "static_task_count": task_count,
        "static_routine_count": routine_count,
        "scratchpad_capacity_bytes": 2 * 1024 * 1024,
        "maximum_tile_spm_bytes": maximum_required_spm,
        "minimum_tile_spm_headroom_bytes": 2 * 1024 * 1024 - maximum_required_spm,
        "maximum_live_global_ram_bytes": counters.get("maximum_live_global_ram_bytes"),
        "physical_dma": {
            "input_requests": counters.get("materialized_input_physical_request_count"),
            "input_bytes": counters.get("materialized_input_physical_byte_count"),
            "output_requests": counters.get("materialized_output_physical_request_count"),
            "output_bytes": counters.get("materialized_output_physical_byte_count"),
        },
        "control_audit_sha256": control_data["audit_sha256"],
        "candidate_audit_sha256": candidate_data["audit_sha256"],
    }


def validate_evidence(
    directory: Path,
    active_tiles: list[int],
    compile_metrics: dict[str, Any],
    compile_run: Path,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if directory.is_symlink() or not directory.is_dir():
        fail(f"SST evidence directory is missing or symbolic: {directory}")
    for relative in EVIDENCE_FILES:
        require_regular_file(directory / relative, f"SST evidence {relative}")

    status = read_csv_row(directory / "status.csv", "SST status")
    result = read_csv_row(directory / "result.csv", "SST result")
    if status.get("status") != "PASS" or status.get("exit_code") != "0":
        fail(f"SST evidence does not carry a successful exit: {directory}")
    if result.get("status") != "PASS":
        fail(f"SST result is not PASS: {directory}")
    expected_result = {
        "active_tiles": str(len(active_tiles)),
        "synchronization_mode": "exact_dependencies",
        "global_ram_channels": "32",
    }
    for field, expected in expected_result.items():
        if result.get(field) != expected:
            fail(f"SST result has a stale {field}: {directory}")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)? (?:ps|ns|us|ms|s)", result.get(
        "simulated_time", ""
    )):
        fail(f"SST result has an invalid simulated time: {directory}")
    try:
        wall_seconds = float(result["simulation_wall_seconds"])
    except (KeyError, ValueError):
        fail(f"SST result has an invalid host-wall time: {directory}")
    if wall_seconds <= 0:
        fail(f"SST result has a nonpositive host-wall time: {directory}")

    usage = parse_resource_usage(directory / "resource-usage.txt")
    if abs(float(usage["sst_wall_seconds"]) - wall_seconds) > 0.01:
        fail(f"SST wall time disagrees with resource usage: {directory}")

    launch = read_json(directory / "launch.json", "SST launch")
    if (
        launch.get("model") != result.get("model")
        or launch.get("global_ram_channels") != 32
        or launch.get("sst_threads") != 16
        or launch.get("sst_partitioner") != "sst.simple"
    ):
        fail(f"SST launch does not use the frozen Phase-0 host policy: {directory}")
    source_compile = launch.get("source_compile_directory")
    if (
        not isinstance(source_compile, str)
        or Path(source_compile).resolve() != compile_run.resolve()
    ):
        fail(f"SST launch is not bound to the expected compile run: {directory}")
    materialization_sha256, _ = digest_file(
        compile_run / "materialization-audit.json"
    )
    run_manifest_sha256, _ = digest_file(compile_run / "run-manifest.json")
    if (
        launch.get("materialization_audit_sha256") != materialization_sha256
        or launch.get("source_run_manifest_sha256") != run_manifest_sha256
    ):
        fail(f"SST launch provenance hash is stale: {directory}")
    launch_semantics = dict(launch)
    launch_semantics.pop("source_compile_directory", None)
    launch_semantics.pop("source_run_manifest_sha256", None)
    launch_semantics.pop("materialization_audit_sha256", None)
    launch_semantics = canonical_json(launch_semantics, directory)

    partition_text = (directory / "partition.txt").read_bytes()
    partition_summary = read_json(
        directory / "partition-summary.json", "SST partition summary"
    )
    if (
        partition_summary.get("status") != "PASS"
        or partition_summary.get("active_tile_count") != len(active_tiles)
        or partition_summary.get("requested_threads") != 16
        or partition_summary.get("occupied_active_tile_threads") != 16
    ):
        fail(f"SST partition summary is incomplete: {directory}")

    progress = read_json(
        directory / "progress-classification.json", "SST progress classification"
    )
    totals = progress.get("totals")
    physical = progress.get("physical_global_dma_work")
    pending = progress.get("pending")
    if (
        progress.get("state") != "passed"
        or progress.get("expected_tiles") != len(active_tiles)
        or progress.get("simulation_pass_tiles") != len(active_tiles)
        or progress.get("initialization_pass_tiles") != len(active_tiles)
        or progress.get("simulation_error_tiles") != []
        or not isinstance(totals, dict)
        or totals.get("instructions", 0) <= 0
        or totals.get("analog_commands_submitted") != totals.get(
            "analog_commands_completed"
        )
        or not isinstance(physical, dict)
        or physical.get("completion_fraction") != 1.0
        or physical.get("remaining_requests") != 0
        or not isinstance(pending, dict)
        or any(value != 0 for value in pending.values())
    ):
        fail(f"SST progress/terminal accounting is incomplete: {directory}")
    progress_semantics = dict(progress)
    progress_semantics.pop("evidence_directory", None)
    progress_semantics.pop("deltas", None)

    log_text = (directory / "simulation.log").read_text(
        encoding="utf-8", errors="replace"
    )
    if (
        log_text.count("SCULPTOR_RA_SIM_PASS") != len(active_tiles)
        or log_text.count("SCULPTOR_RA_INIT_PASS") != len(active_tiles)
        or "SCULPTOR_RA_SIM_FAIL" in log_text
        or "Simulation failed" in log_text
    ):
        fail(f"SST terminal log is incomplete or contains failure: {directory}")
    stop_counts = parse_stop_counts(log_text)

    stats_header, stats_rows = router_statistics_rows(
        directory / "router-statistics.csv"
    )
    stats_sha256 = hashlib.sha256(
        json.dumps([stats_header, stats_rows], separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    stats_summary = summarize_statistics(stats_header, stats_rows)
    expected_dma = compile_metrics["physical_dma"]
    expected_requests = expected_dma["input_requests"] + expected_dma["output_requests"]
    expected_bytes = expected_dma["input_bytes"] + expected_dma["output_bytes"]
    if (
        stats_summary["requests"] != physical.get("completed_requests")
        or stats_summary["requests"] != expected_requests
        or stats_summary["bytes"] != expected_bytes
    ):
        fail(f"SST RAM statistics do not reconcile with physical work: {directory}")

    status_semantics = dict(status)
    status_semantics.pop("total_wall_seconds", None)
    result_semantics = dict(result)
    result_semantics.pop("simulation_wall_seconds", None)
    identity = {
        "launch": launch_semantics,
        "partition_sha256": hashlib.sha256(partition_text).hexdigest(),
        "partition_summary": partition_summary,
        "progress": progress_semantics,
        "result": result_semantics,
        "router_statistics_sha256": stats_sha256,
        "status": status_semantics,
        "stop_counts": stop_counts,
    }
    metrics = {
        "directory": str(directory.resolve()),
        "simulated_time": result["simulated_time"],
        "wall_seconds": wall_seconds,
        "maximum_rss_kib": usage["sst_max_rss_kib"],
        "runtime_totals": totals,
        "global_ram": stats_summary,
        "stop_counts": stop_counts,
    }
    return identity, metrics


def validate_phase_zero_differential(args: argparse.Namespace) -> dict[str, Any]:
    control = args.control_run_directory
    candidate = args.candidate_run_directory
    if control is None or candidate is None:
        fail("Phase-0 A/B validation requires both compile run directories")
    active_tiles, compile_metrics = compare_compile_runs(control, candidate, args)
    if compile_metrics["candidate_phase"] != 0:
        fail(
            "completed Phase-0 SST differential requires a Phase-0 analyze "
            "candidate; use --compile-only-differential for Phase 2"
        )

    minimum = args.minimum_evidence_observations
    if (
        len(args.control_evidence_directory) < minimum
        or len(args.candidate_evidence_directory) < minimum
    ):
        fail(f"Phase-0 A/B validation requires at least {minimum} observations per mode")
    identities: list[dict[str, Any]] = []
    control_metrics: list[dict[str, Any]] = []
    candidate_metrics: list[dict[str, Any]] = []
    for directory in args.control_evidence_directory:
        identity, metrics = validate_evidence(
            directory, active_tiles, compile_metrics, control
        )
        identities.append(identity)
        control_metrics.append(metrics)
    for directory in args.candidate_evidence_directory:
        identity, metrics = validate_evidence(
            directory, active_tiles, compile_metrics, candidate
        )
        identities.append(identity)
        candidate_metrics.append(metrics)
    reference = identities[0]
    for identity in identities[1:]:
        if identity != reference:
            fail(
                "Phase-0 completed SST semantics or physical accounting differ: "
                + first_difference(reference, identity)
            )

    def wall_summary(observations: list[dict[str, Any]]) -> dict[str, Any]:
        values = [float(observation["wall_seconds"]) for observation in observations]
        return {
            "observations": values,
            "median": statistics.median(values),
            "mean": statistics.fmean(values),
            "minimum": min(values),
            "maximum": max(values),
        }

    return {
        "schema": "sculptor.execution-residency-phase0-differential",
        "schema_version": 1,
        "status": "PASS",
        "control_mode": "off",
        "candidate_mode": "analyze",
        "compile": compile_metrics,
        "sst": {
            "observation_count_per_mode": {
                "off": len(control_metrics),
                "analyze": len(candidate_metrics),
            },
            "simulated_time": reference["result"]["simulated_time"],
            "off_wall_seconds": wall_summary(control_metrics),
            "analyze_wall_seconds": wall_summary(candidate_metrics),
            "runtime_totals": control_metrics[0]["runtime_totals"],
            "global_ram": control_metrics[0]["global_ram"],
            "router_statistics_sha256": reference["router_statistics_sha256"],
            "partition_sha256": reference["partition_sha256"],
            "stop_counts": reference["stop_counts"],
            "maximum_rss_kib": {
                "off": [item["maximum_rss_kib"] for item in control_metrics],
                "analyze": [item["maximum_rss_kib"] for item in candidate_metrics],
            },
        },
    }


def validate_compile_differential(args: argparse.Namespace) -> dict[str, Any]:
    control = args.control_run_directory
    candidate = args.candidate_run_directory
    if control is None or candidate is None:
        fail("compile A/B validation requires both compile run directories")
    _, compile_metrics = compare_compile_runs(control, candidate, args)
    return {
        "schema": "sculptor.execution-residency-compile-differential",
        "schema_version": 1,
        "status": "PASS",
        "control_mode": "off",
        "candidate_mode": "analyze",
        "control_phase": compile_metrics["control_phase"],
        "candidate_phase": compile_metrics["candidate_phase"],
        "compile": compile_metrics,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--input-mlir", type=Path)
    parser.add_argument("--control-mlir", type=Path)
    parser.add_argument("--output-mlir", type=Path)
    parser.add_argument("--expected-mode", required=True)
    parser.add_argument("--expected-maximum-members", type=int, required=True)
    parser.add_argument("--expected-maximum-wave-width", type=int, required=True)
    parser.add_argument(
        "--expected-require-positive-benefit", type=parse_bool, required=True
    )
    parser.add_argument("--control-run-directory", type=Path)
    parser.add_argument("--candidate-run-directory", type=Path)
    parser.add_argument(
        "--compile-only-differential",
        action="store_true",
        help=(
            "validate compile artifacts and physical accounting without "
            "requiring SST observations"
        ),
    )
    parser.add_argument(
        "--control-evidence-directory", type=Path, action="append", default=[]
    )
    parser.add_argument(
        "--candidate-evidence-directory", type=Path, action="append", default=[]
    )
    parser.add_argument("--minimum-evidence-observations", type=int, default=3)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.expected_maximum_members <= 0:
        parser.error("--expected-maximum-members must be positive")
    if not 1 <= args.expected_maximum_wave_width <= 8:
        parser.error("--expected-maximum-wave-width must be in [1, 8]")
    if args.minimum_evidence_observations < 1:
        parser.error("--minimum-evidence-observations must be positive")
    bundle_requested = any(
        (
            args.control_run_directory is not None,
            args.candidate_run_directory is not None,
            bool(args.control_evidence_directory),
            bool(args.candidate_evidence_directory),
        )
    )
    if bundle_requested and (
        args.control_run_directory is None or args.candidate_run_directory is None
    ):
        parser.error(
            "Phase-0 A/B validation requires --control-run-directory and "
            "--candidate-run-directory"
        )
    if args.compile_only_differential and not bundle_requested:
        parser.error(
            "--compile-only-differential requires both compile run directories"
        )
    if args.compile_only_differential and (
        args.control_evidence_directory or args.candidate_evidence_directory
    ):
        parser.error(
            "--compile-only-differential cannot be combined with SST evidence"
        )
    return args


def main() -> int:
    try:
        args = parse_args()
        result = validate(args)
        if args.control_run_directory is not None:
            if args.compile_only_differential:
                result["compile_differential"] = validate_compile_differential(args)
            else:
                result["phase0_differential"] = validate_phase_zero_differential(args)
        if args.report is not None:
            if args.report.is_symlink():
                fail(f"report path is symbolic: {args.report}")
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except (ValidationError, OSError) as error:
        print(f"execution-residency validation: FAIL: {error}", file=sys.stderr)
        return 1
    json.dump(result, sys.stdout, sort_keys=True, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
