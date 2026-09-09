#!/usr/bin/env python3
"""Validate FAST Phase-4 late physical certificates and byte equivalence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


class ValidationError(RuntimeError):
    pass


DESCRIPTORS = "sculptor.materialization.dma_descriptors"
SEGMENTS = "sculptor.materialization.dma_segments"
PHYSICAL_REGIONS = "sculptor.execution_residency.physical_regions"
PHYSICAL_EDGES = "sculptor.execution_residency.physical_internal_edges"
REGION_CERTIFICATES = "sculptor.execution_residency.physical_certificates"
EDGE_CERTIFICATES = "sculptor.execution_residency.physical_edge_certificates"


def fail(message: str) -> None:
    raise ValidationError(message)


def extract_array(text: str, name: str) -> str:
    marker = f"{name} = "
    offsets = [match.start() for match in re.finditer(re.escape(marker), text)]
    if len(offsets) != 1:
        fail(f"expected one {name} attribute, found {len(offsets)}")
    begin = offsets[0] + len(marker)
    if begin >= len(text) or text[begin] != "[":
        fail(f"{name} is not an array")
    depth = 0
    quoted = False
    escaped = False
    for index in range(begin, len(text)):
        character = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
        elif character == "[":
            depth += 1
        elif character == "]":
            depth -= 1
            if depth == 0:
                return text[begin : index + 1]
    fail(f"{name} has an unterminated array")


def dictionaries(array: str, name: str) -> list[str]:
    result: list[str] = []
    depth = 0
    begin = -1
    quoted = False
    escaped = False
    for index, character in enumerate(array):
        if quoted:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
        elif character == "{":
            if depth == 0:
                begin = index
            depth += 1
        elif character == "}":
            depth -= 1
            if depth < 0:
                fail(f"{name} has unbalanced dictionaries")
            if depth == 0:
                result.append(array[begin : index + 1])
    if depth != 0:
        fail(f"{name} has an unterminated dictionary")
    return result


def typed_count(array: str, spelling: str) -> int:
    return array.count(f"#sculptor.{spelling}<")


def i64(record: str, field: str) -> int:
    match = re.search(rf"\b{re.escape(field)} = (-?\d+) : i64\b", record)
    if match is None:
        fail(f"certificate is missing i64 field {field}")
    return int(match.group(1))


def string(record: str, field: str) -> str:
    match = re.search(rf'\b{re.escape(field)} = "([^"]*)"', record)
    if match is None:
        fail(f"certificate is missing string field {field}")
    return match.group(1)


def i64_array(record: str, field: str) -> list[int]:
    match = re.search(rf"\b{re.escape(field)} = \[([^]]*)\]", record)
    if match is None:
        fail(f"certificate is missing array field {field}")
    body = match.group(1).strip()
    if not body:
        return []
    try:
        return [int(value.strip()) for value in body.split(",")]
    except ValueError as error:
        raise ValidationError(
            f"certificate field {field} is not an integer array"
        ) from error


def manifest_ids(directory: Path) -> list[int]:
    path = directory / "active-cores.txt"
    try:
        values = [
            int(line.strip())
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    except (OSError, ValueError) as error:
        raise ValidationError(f"cannot read active-core manifest {path}: {error}") from error
    if not values or values != sorted(set(values)):
        fail(f"active-core manifest is empty, unsorted, or duplicated: {path}")
    return values


def memory_summary(directory: Path, active: list[int], capacity: int) -> dict[str, Any]:
    path = directory / "memory-reports" / "tile-memory-summary.json"
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot read memory summary {path}: {error}") from error
    if report.get("active_tile_count") != len(active):
        fail("memory summary active-tile count disagrees with its manifest")
    if report.get("scratchpad_capacity_bytes") != capacity:
        fail("memory summary scratchpad capacity disagrees with the gate")
    if report.get("capacity_gate", {}).get("status") != "PASS":
        fail("tile-memory capacity gate did not pass")
    maxima = report.get("summaries", {}).get("capacity", {}).get("maximums", {})
    required = maxima.get("requiredLocalBytes")
    workspace = maxima.get("workspaceBytes")
    if not isinstance(required, int) or required < 0 or required > capacity:
        fail("maximum required local bytes exceed physical SPM capacity")
    if workspace != 0:
        fail("a tile-memory allocation reaches QEMU workspace")
    return {
        "path": str(path),
        "maximum_required_local_bytes": required,
        "maximum_workspace_bytes": workspace,
    }


def validate_certificate(
    record: str, tile: int, capacity: int, rings: dict[int, int]
) -> None:
    if i64(record, "schema_version") != 1:
        fail("region certificate has a stale schema version")
    if i64(record, "physical_tile_id") != tile:
        fail("region certificate names the wrong physical tile")
    actual_ring = i64(record, "actual_ring_slots")
    minimum_ring = i64(record, "minimum_member_ring_slots")
    maximum_ring = i64(record, "maximum_member_ring_slots")
    if actual_ring <= 0 or actual_ring != minimum_ring or maximum_ring < actual_ring:
        fail("region certificate ring geometry is inconsistent")
    rings[actual_ring] = rings.get(actual_ring, 0) + 1
    required = i64(record, "actual_tile_required_local_bytes")
    peak = i64(record, "actual_tile_peak_live_bytes")
    if required < 0 or required > capacity or peak < 0 or peak > required:
        fail("region certificate exceeds its exact tile-memory capacity")
    if i64(record, "member_count") <= 0 or i64(record, "internal_edge_count") <= 0:
        fail("region certificate is structurally empty")
    if i64(record, "elided_request_count") != 0 or i64(record, "elided_bytes") != 0:
        fail("Phase 4 must not claim physical traffic elimination")


def validate_edge(record: str, dispositions: dict[str, int]) -> None:
    if i64(record, "schema_version") != 1:
        fail("edge certificate has a stale schema version")
    for field in (
        "source_operation_id",
        "source_work_unit_id",
        "source_result_number",
        "source_epoch_id",
        "target_operation_id",
        "target_work_unit_id",
        "target_operand_number",
        "target_epoch_id",
        "tensor_id",
    ):
        if i64(record, field) < 0:
            fail(f"edge certificate has an invalid {field}")
    if i64(record, "source_epoch_id") == 0 or i64(record, "target_epoch_id") == 0:
        fail("edge certificate has an invalid semantic epoch")
    if not i64_array(record, "boundary_ids"):
        fail("edge certificate has no authoritative boundary identity")
    source_owners = i64_array(record, "source_owner_ids")
    target_owners = i64_array(record, "target_owner_ids")
    shared_owners = i64_array(record, "shared_owner_ids")
    if not source_owners or not target_owners:
        fail("edge certificate has no exact endpoint owners")
    if not i64_array(record, "source_runtime_output_ports") or not i64_array(
        record, "target_runtime_input_ports"
    ):
        fail("edge certificate has no exact runtime port identity")
    disposition = string(record, "disposition")
    dispositions[disposition] = dispositions.get(disposition, 0) + 1
    if disposition == "owner_alias" and not shared_owners:
        fail("owner-alias certificate has no shared physical owner")
    if disposition != "owner_alias" and shared_owners:
        fail("materialized/local edge unexpectedly aliases one owner")
    for prefix in ("fill", "spill"):
        descriptor_ids = i64_array(record, f"{prefix}_descriptor_ids")
        if i64(record, f"{prefix}_descriptor_count") != len(descriptor_ids):
            fail(f"{prefix} descriptor count disagrees with its exact IDs")
        if i64(record, f"{prefix}_request_count") < 0 or i64(
            record, f"{prefix}_bytes"
        ) < 0:
            fail(f"{prefix} transition accounting is negative")


def validate(args: argparse.Namespace) -> dict[str, Any]:
    phase3_active = manifest_ids(args.phase3_selected_compile)
    phase4_active = manifest_ids(args.phase4_compile)
    if phase3_active != phase4_active:
        fail("Phase-3 and Phase-4 active-core manifests differ")
    if args.expected_active_tiles is not None and len(phase4_active) != args.expected_active_tiles:
        fail("active-tile count disagrees with the expected qualification")
    memory = memory_summary(args.phase4_compile, phase4_active, args.scratchpad_capacity)

    inventory_hash = hashlib.sha256()
    region_count = 0
    edge_count = 0
    rings: dict[int, int] = {}
    dispositions: dict[str, int] = {}
    for tile in phase4_active:
        phase3_path = args.phase3_selected_compile / "cores" / f"core-{tile}-finalized.mlir"
        phase4_path = args.phase4_compile / "cores" / f"core-{tile}-finalized.mlir"
        try:
            phase3 = phase3_path.read_text(encoding="utf-8")
            phase4 = phase4_path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValidationError(f"cannot read finalized tile {tile}: {error}") from error
        if not (args.phase4_compile / "cores" / f"core-{tile}.o").is_file():
            fail(f"tile {tile} has no compiled RISC-V object")
        for name in (DESCRIPTORS, SEGMENTS):
            control = extract_array(phase3, name)
            selected = extract_array(phase4, name)
            if control != selected:
                fail(f"tile {tile} {name} changed relative to Phase 3")
            inventory_hash.update(f"{tile}:{name}:".encode())
            inventory_hash.update(selected.encode())

        seed_regions = typed_count(
            extract_array(phase4, PHYSICAL_REGIONS),
            "execution_residency_region",
        )
        seed_edges = typed_count(
            extract_array(phase4, PHYSICAL_EDGES),
            "execution_residency_internal_edge",
        )
        region_records = dictionaries(
            extract_array(phase4, REGION_CERTIFICATES), REGION_CERTIFICATES
        )
        edge_records = dictionaries(
            extract_array(phase4, EDGE_CERTIFICATES), EDGE_CERTIFICATES
        )
        if len(region_records) != seed_regions or len(edge_records) != seed_edges:
            fail(f"tile {tile} late certificate does not cover its complete early plan")
        for record in region_records:
            validate_certificate(record, tile, args.scratchpad_capacity, rings)
        for record in edge_records:
            validate_edge(record, dispositions)
        region_count += len(region_records)
        edge_count += len(edge_records)

    if args.expected_regions is not None and region_count != args.expected_regions:
        fail("physical region count disagrees with the expected qualification")
    if args.expected_edges is not None and edge_count != args.expected_edges:
        fail("physical edge count disagrees with the expected qualification")
    if args.expected_two_slot_regions is not None and rings.get(2, 0) != args.expected_two_slot_regions:
        fail("two-slot region count disagrees with the expected qualification")
    if not rings.get(1, 0) or not rings.get(2, 0):
        fail("qualification did not exercise both one-slot and two-slot regions")

    return {
        "schema": "golem.sculptor-fast-phase4-physical-plan",
        "schema_version": 1,
        "status": "PASS",
        "active_tile_count": len(phase4_active),
        "physical_region_count": region_count,
        "physical_edge_count": edge_count,
        "ring_slot_histogram": {str(key): value for key, value in sorted(rings.items())},
        "edge_disposition_histogram": dict(sorted(dispositions.items())),
        "materialized_inventory_sha256": inventory_hash.hexdigest(),
        "materialized_inventory_equivalent_to_phase3": True,
        "memory": memory,
        "errors": [],
    }


def write_report(path: Path, report: dict[str, Any]) -> None:
    if path.is_symlink():
        fail(f"report path is symbolic: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase3-selected-compile", type=Path, required=True)
    parser.add_argument("--phase4-compile", type=Path, required=True)
    parser.add_argument("--scratchpad-capacity", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--expected-active-tiles", type=int)
    parser.add_argument("--expected-regions", type=int)
    parser.add_argument("--expected-edges", type=int)
    parser.add_argument("--expected-two-slot-regions", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = validate(args)
        write_report(args.output, report)
    except (ValidationError, OSError) as error:
        print(f"FAST Phase-4 physical plan: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "FAST Phase-4 physical plan: PASS "
        f"tiles={report['active_tile_count']} "
        f"regions={report['physical_region_count']} "
        f"edges={report['physical_edge_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
