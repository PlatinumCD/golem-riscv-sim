#!/usr/bin/env python3
"""Validate finalized per-tile V1 materialization audits.

The model compiler copies the model-wide ownership audit into every extracted
tile.  FinalizeTileRuntimeGraph then replaces the two DMA descriptor counters
with tile-local values.  This validator proves that the model-wide fields are
identical, aggregates the local counters, and emits a persistent PASS/FAIL
certificate before an eight-model run may claim ``COMPILE_PASS``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tests" / "support"))
from deployment_manifest import load_deployment_manifest  # noqa: E402


AUDIT_ATTRIBUTE = "sculptor.materialization.audit"
DESCRIPTOR_ATTRIBUTE = "sculptor.materialization.dma_descriptors"
SEGMENT_ATTRIBUTE = "sculptor.materialization.dma_segments"
FINALIZED_PATTERN = re.compile(r"core-([0-9]+)-finalized\.mlir\Z")
INTEGER_FIELD = re.compile(
    r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?[0-9]+)\s*:\s*i64\s*\Z"
)

AUDIT_FIELDS = (
    "schema_version",
    "epoch_count",
    "materialized_tensor_count",
    "materialized_tensor_bytes",
    "materialized_producer_region_count",
    "materialized_consumer_region_count",
    "zero_contribution_consumer_region_count",
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
OPTIONAL_GLOBAL_COUNTERS = (
    # FormParametricShards records conservative producer joins that must be
    # discharged by PlanTensorShards.  Older schema-v1 artifacts predate this
    # accounting field, so accept its absence as zero while treating any
    # nonzero value in a finalized deployment as a correctness failure.
    "deferred_dependency_count",
)
OPTIONAL_LOCAL_COUNTERS = (
    "retained_local_owner_alias_count",
    "elided_retained_input_descriptor_count",
    "elided_retained_output_descriptor_count",
    "elided_retained_input_logical_bytes",
    "elided_retained_output_logical_bytes",
    "elided_retained_input_logical_transfer_count",
    "elided_retained_output_logical_transfer_count",
)
PHASE4_LOCAL_COUNTERS = (
    "retained_local_policy_enabled_tile_count",
    "retained_local_eligible_component_count",
    "retained_local_selected_component_count",
    "retained_local_fallback_component_count",
    "retained_local_mixed_lattice_fallback_component_count",
    "retained_local_epoch_closure_fallback_component_count",
    "retained_local_epoch_component_count",
    "retained_local_epoch_coalesced_kernel_count",
    "retained_local_owner_count",
    "retained_local_route_count",
    "retained_local_logical_bytes",
    "retained_local_logical_transfer_count",
    "elided_retained_input_physical_request_count",
    "elided_retained_input_physical_byte_count",
    "elided_retained_output_physical_request_count",
    "elided_retained_output_physical_byte_count",
    "materialized_input_physical_request_count",
    "materialized_input_physical_byte_count",
    "materialized_output_physical_request_count",
    "materialized_output_physical_byte_count",
    "retained_input_descriptor_count_before_elision",
    "retained_output_descriptor_count_before_elision",
    "retained_input_logical_bytes_before_elision",
    "retained_output_logical_bytes_before_elision",
    "retained_input_logical_transfer_count_before_elision",
    "retained_output_logical_transfer_count_before_elision",
    "retained_input_physical_request_count_before_elision",
    "retained_input_physical_byte_count_before_elision",
    "retained_output_physical_request_count_before_elision",
    "retained_output_physical_byte_count_before_elision",
    "retained_local_hybrid_fanout_count",
    "retained_local_capacity_fallback_count",
    "preserved_hybrid_spill_descriptor_count",
    "preserved_external_spill_descriptor_count",
    "final_required_local_bytes",
    "capacity_bytes",
    "capacity_headroom_bytes",
)
PHASE5_LOCAL_COUNTERS = (
    "direct_forward_selected_route_count",
    "direct_forward_elided_input_descriptor_count",
    "direct_forward_elided_input_logical_bytes",
    "direct_forward_elided_input_logical_transfer_count",
    "direct_forward_elided_input_physical_byte_count",
    "direct_forward_elided_input_physical_request_count",
    "direct_forward_elided_output_descriptor_count",
    "direct_forward_elided_output_logical_bytes",
    "direct_forward_elided_output_logical_transfer_count",
    "direct_forward_elided_output_physical_byte_count",
    "direct_forward_elided_output_physical_request_count",
    "direct_forward_preserved_hybrid_spill_descriptor_count",
)
KNOWN_AUDIT_FIELDS = (
    AUDIT_FIELDS
    + OPTIONAL_GLOBAL_COUNTERS
    + OPTIONAL_LOCAL_COUNTERS
    + PHASE4_LOCAL_COUNTERS
    + PHASE5_LOCAL_COUNTERS
)
LOCAL_COUNTERS = {
    "materialized_output_dma_descriptor_count",
    "materialized_input_dma_descriptor_count",
    *OPTIONAL_LOCAL_COUNTERS,
    *PHASE4_LOCAL_COUNTERS,
    *PHASE5_LOCAL_COUNTERS,
}
GLOBAL_FIELDS = tuple(
    field
    for field in AUDIT_FIELDS + OPTIONAL_GLOBAL_COUNTERS
    if field not in LOCAL_COUNTERS
)
ZERO_CORRECTNESS_FIELDS = (
    "unowned_materialized_byte_count",
    "multiply_owned_materialized_byte_count",
    "read_before_produced_region_count",
    "cross_epoch_direct_route_count",
    "unclassified_boundary_count",
    "deferred_dependency_count",
)


class AuditError(RuntimeError):
    """A malformed or inconsistent materialization contract."""


UINT64_MAX = (1 << 64) - 1


def checked_add_u64(left: int, right: int, context: str) -> int:
    if left < 0 or right < 0 or left > UINT64_MAX - right:
        raise AuditError(f"unsigned 64-bit addition overflow while {context}")
    return left + right


def checked_multiply_u64(left: int, right: int, context: str) -> int:
    if left < 0 or right < 0 or (left != 0 and right > UINT64_MAX // left):
        raise AuditError(f"unsigned 64-bit multiplication overflow while {context}")
    return left * right


def add_histogram_count(
    histogram: dict[int, int], byte_size: int, count: int, context: str
) -> None:
    histogram[byte_size] = checked_add_u64(
        histogram.get(byte_size, 0), count, context
    )


def descriptor_occurrence_count(descriptor: dict[str, int]) -> int:
    begin = descriptor["iterationBegin"]
    end = descriptor["iterationEnd"]
    step = descriptor["iterationStep"]
    if begin < 0 or end <= begin or step <= 0:
        raise AuditError("cannot count an invalid materialized DMA interval")
    return 1 + (end - 1 - begin) // step


def is_single_request_input_descriptor(
    descriptor: dict[str, int],
    segments: list[dict[str, int]],
    maximum_frame_bytes: int,
) -> bool:
    offset = descriptor["segmentOffset"]
    if (
        descriptor["direction"] != 0
        or descriptor["segmentCount"] != 1
        or offset < 0
        or offset >= len(segments)
        or not 0 < descriptor["bytesPerIteration"] <= maximum_frame_bytes
    ):
        return False
    segment = segments[offset]
    return (
        segment["descriptorId"] == descriptor["id"]
        and segment["repeatCount"] == 1
        and segment["pieceCount"] == 1
        and segment["byteSize"] == descriptor["bytesPerIteration"]
    )


def is_canonical_input_run_join(
    left_index: int,
    right_index: int,
    descriptors: list[dict[str, int]],
    segments: list[dict[str, int]],
    maximum_frame_bytes: int,
) -> bool:
    """Prove one order-preserving join without using compiler role flags."""

    if right_index != left_index + 1:
        return False
    left = descriptors[left_index]
    right = descriptors[right_index]
    compact_flags = (1 << 4) | (1 << 5)
    if (
        not is_single_request_input_descriptor(
            left, segments, maximum_frame_bytes
        )
        or not is_single_request_input_descriptor(
            right, segments, maximum_frame_bytes
        )
        or right["id"] != left["id"] + 1
        or (left["flags"] & ~compact_flags)
        != (right["flags"] & ~compact_flags)
        or any(
            left[name] != right[name]
            for name in (
                "operationId",
                "epochId",
                "workUnitId",
                "tensorId",
                "loopId",
                "templateKind",
                "iterationBegin",
                "iterationEnd",
                "iterationStep",
                "scratchpadSlotStride",
                "ringSlots",
            )
        )
        or right["portNumber"] != left["portNumber"] + 1
        or right["segmentOffset"] != left["segmentOffset"] + 1
    ):
        return False

    left_segment = segments[left["segmentOffset"]]
    right_segment = segments[right["segmentOffset"]]
    if any(
        left_segment[name] != right_segment[name]
        for name in (
            "relationId",
            "relationPieceOrdinal",
            "globalResourceId",
            "globalIterationStride",
            "scratchpadIterationStride",
            "globalRepeatStride",
            "scratchpadRepeatStride",
            "globalPieceStride",
            "scratchpadPieceStride",
        )
    ):
        return False
    left_global_end = checked_add_u64(
        left_segment["globalByteOffset"],
        left_segment["byteSize"],
        "proving a maximal input-DMA global join",
    )
    left_scratchpad = checked_add_u64(
        left["scratchpadRingBase"],
        left_segment["scratchpadByteOffset"],
        "proving a maximal input-DMA scratchpad join",
    )
    left_scratchpad_end = checked_add_u64(
        left_scratchpad,
        left_segment["byteSize"],
        "proving a maximal input-DMA scratchpad join",
    )
    right_scratchpad = checked_add_u64(
        right["scratchpadRingBase"],
        right_segment["scratchpadByteOffset"],
        "proving a maximal input-DMA scratchpad join",
    )
    return (
        left_global_end == right_segment["globalByteOffset"]
        and left_scratchpad_end == right_scratchpad
    )


def maximal_input_dma_run_audit(
    descriptors: list[dict[str, int]],
    segments: list[dict[str, int]],
    maximum_frame_bytes: int,
    current_request_count: int,
    current_byte_count: int,
) -> dict[str, Any]:
    """Build the exact left-to-right maximal <=frame request packing audit."""

    static_run_count = 0
    static_descriptor_count = 0
    maximum_run_descriptor_count = 0
    maximum_run_bytes = 0
    dynamic_packed_request_count = 0
    run_length_histogram: dict[int, int] = {}
    candidate_request_size_histogram: dict[int, int] = {}

    index = 0
    while index < len(descriptors):
        descriptor = descriptors[index]
        if descriptor["direction"] != 0:
            index += 1
            continue
        occurrence_count = descriptor_occurrence_count(descriptor)
        if not is_single_request_input_descriptor(
            descriptor, segments, maximum_frame_bytes
        ):
            segment_begin = descriptor["segmentOffset"]
            segment_end = segment_begin + descriptor["segmentCount"]
            for segment in segments[segment_begin:segment_end]:
                expanded_count = checked_multiply_u64(
                    segment["repeatCount"],
                    segment["pieceCount"],
                    "auditing an unpacked input-DMA segment",
                )
                expanded_count = checked_multiply_u64(
                    expanded_count,
                    occurrence_count,
                    "auditing an unpacked input-DMA descriptor",
                )
                add_histogram_count(
                    candidate_request_size_histogram,
                    segment["byteSize"],
                    expanded_count,
                    "building the maximal input-DMA candidate histogram",
                )
            index += 1
            continue

        run_begin = index
        run_bytes = descriptor["bytesPerIteration"]
        index += 1
        while index < len(descriptors):
            next_bytes = descriptors[index]["bytesPerIteration"]
            if (
                not is_canonical_input_run_join(
                    index - 1,
                    index,
                    descriptors,
                    segments,
                    maximum_frame_bytes,
                )
                or next_bytes > maximum_frame_bytes - run_bytes
            ):
                break
            run_bytes += next_bytes
            index += 1

        run_descriptor_count = index - run_begin
        add_histogram_count(
            candidate_request_size_histogram,
            run_bytes,
            occurrence_count,
            "building the maximal input-DMA candidate histogram",
        )
        if run_descriptor_count < 2:
            continue
        static_run_count = checked_add_u64(
            static_run_count, 1, "counting maximal input-DMA runs"
        )
        static_descriptor_count = checked_add_u64(
            static_descriptor_count,
            run_descriptor_count,
            "counting descriptors in maximal input-DMA runs",
        )
        dynamic_packed_request_count = checked_add_u64(
            dynamic_packed_request_count,
            occurrence_count,
            "counting dynamic maximal input-DMA requests",
        )
        maximum_run_descriptor_count = max(
            maximum_run_descriptor_count, run_descriptor_count
        )
        maximum_run_bytes = max(maximum_run_bytes, run_bytes)
        add_histogram_count(
            run_length_histogram,
            run_descriptor_count,
            1,
            "building the maximal input-DMA run-length histogram",
        )

    predicted_request_count = 0
    predicted_byte_count = 0
    for byte_size, count in candidate_request_size_histogram.items():
        predicted_request_count = checked_add_u64(
            predicted_request_count,
            count,
            "reconciling maximal input-DMA candidate requests",
        )
        predicted_byte_count = checked_add_u64(
            predicted_byte_count,
            checked_multiply_u64(
                byte_size,
                count,
                "reconciling maximal input-DMA candidate bytes",
            ),
            "reconciling maximal input-DMA candidate bytes",
        )
    if predicted_byte_count != current_byte_count:
        raise AuditError(
            "maximal input-DMA run packing changed physical byte accounting"
        )
    if predicted_request_count > current_request_count:
        raise AuditError(
            "maximal input-DMA run packing increased physical request count"
        )
    return {
        "static_run_count": static_run_count,
        "static_descriptor_count": static_descriptor_count,
        "maximum_run_descriptor_count": maximum_run_descriptor_count,
        "maximum_run_bytes": maximum_run_bytes,
        "dynamic_packed_request_count": dynamic_packed_request_count,
        "predicted_physical_request_count": predicted_request_count,
        "predicted_additional_request_reduction": current_request_count
        - predicted_request_count,
        "run_length_histogram": run_length_histogram,
        "candidate_request_size_histogram": candidate_request_size_histogram,
    }


def add_packed_sequence_histogram(
    histogram: dict[int, int],
    byte_size: int,
    logical_transfer_count: int,
    sequence_count: int,
    maximum_frame_bytes: int,
    context: str,
) -> int:
    transfers_per_request = maximum_frame_bytes // byte_size
    if transfers_per_request == 0:
        raise AuditError(f"cannot pack an oversized physical transfer while {context}")
    full_requests, tail_transfers = divmod(
        logical_transfer_count, transfers_per_request
    )
    if full_requests:
        add_histogram_count(
            histogram,
            transfers_per_request * byte_size,
            checked_multiply_u64(full_requests, sequence_count, context),
            context,
        )
    if tail_transfers:
        add_histogram_count(
            histogram,
            tail_transfers * byte_size,
            sequence_count,
            context,
        )
    requests_per_sequence = full_requests + int(tail_transfers != 0)
    return checked_multiply_u64(requests_per_sequence, sequence_count, context)


def affine_segment_run_audit(
    descriptors: list[dict[str, int]],
    segments: list[dict[str, int]],
    maximum_frame_bytes: int,
    current_accounting: dict[str, Any],
) -> dict[str, Any]:
    """Audit exact repeat-major/piece-minor packing inside affine segments."""

    results: dict[str, Any] = {}
    compact_leader_flag = 1 << 4
    compact_follower_flag = 1 << 5
    compact_flags = compact_leader_flag | compact_follower_flag
    for direction_name, direction_value in (("input", 0), ("output", 1)):
        candidate_histogram: dict[int, int] = {}
        static_packable_segment_count = 0
        static_full_linear_segment_count = 0
        static_piece_linear_segment_count = 0
        static_repeat_linear_segment_count = 0
        dynamic_logical_request_count = 0
        dynamic_packed_request_count = 0
        maximum_logical_transfers_per_request = 1

        for descriptor in descriptors:
            if descriptor["direction"] != direction_value:
                continue
            occurrence_count = descriptor_occurrence_count(descriptor)
            role = descriptor["flags"] & compact_flags
            if role == compact_follower_flag:
                continue
            if role == compact_leader_flag:
                follower_index = descriptor["id"] + 1
                if follower_index >= len(descriptors):
                    raise AuditError(
                        "compact input leader is missing while auditing affine runs"
                    )
                combined_bytes = checked_add_u64(
                    descriptor["bytesPerIteration"],
                    descriptors[follower_index]["bytesPerIteration"],
                    "preserving a compact descriptor pair in the affine-run audit",
                )
                add_histogram_count(
                    candidate_histogram,
                    combined_bytes,
                    occurrence_count,
                    "preserving compact descriptor pairs in the affine-run audit",
                )
                continue

            segment_begin = descriptor["segmentOffset"]
            segment_end = segment_begin + descriptor["segmentCount"]
            for segment in segments[segment_begin:segment_end]:
                repeat_count = segment["repeatCount"]
                piece_count = segment["pieceCount"]
                byte_size = segment["byteSize"]
                transfers_per_request = maximum_frame_bytes // byte_size
                logical_transfers = checked_multiply_u64(
                    repeat_count,
                    piece_count,
                    "auditing affine-segment logical transfers",
                )
                piece_linear = (
                    piece_count > 1
                    and segment["globalPieceStride"] == byte_size
                    and segment["scratchpadPieceStride"] == byte_size
                )
                repeat_linear = (
                    repeat_count > 1
                    and segment["globalRepeatStride"]
                    == piece_count * byte_size
                    and segment["scratchpadRepeatStride"]
                    == piece_count * byte_size
                )
                full_linear = repeat_linear and (piece_count == 1 or piece_linear)

                packed_requests_per_occurrence = logical_transfers
                strategy = "none"
                candidate_width = 1
                if transfers_per_request > 1 and full_linear:
                    local_histogram: dict[int, int] = {}
                    packed_requests_per_occurrence = add_packed_sequence_histogram(
                        local_histogram,
                        byte_size,
                        logical_transfers,
                        1,
                        maximum_frame_bytes,
                        "packing one fully linear affine DMA segment",
                    )
                    strategy = "full"
                    candidate_width = min(
                        transfers_per_request, logical_transfers
                    )
                elif transfers_per_request > 1 and piece_linear:
                    local_histogram = {}
                    packed_requests_per_occurrence = add_packed_sequence_histogram(
                        local_histogram,
                        byte_size,
                        piece_count,
                        repeat_count,
                        maximum_frame_bytes,
                        "packing piece-linear affine DMA rows",
                    )
                    strategy = "piece"
                    candidate_width = min(transfers_per_request, piece_count)
                elif (
                    transfers_per_request > 1
                    and piece_count == 1
                    and repeat_linear
                ):
                    local_histogram = {}
                    packed_requests_per_occurrence = add_packed_sequence_histogram(
                        local_histogram,
                        byte_size,
                        repeat_count,
                        1,
                        maximum_frame_bytes,
                        "packing a repeat-linear affine DMA segment",
                    )
                    strategy = "repeat"
                    candidate_width = min(transfers_per_request, repeat_count)
                else:
                    local_histogram = {byte_size: logical_transfers}

                for request_bytes, count in local_histogram.items():
                    add_histogram_count(
                        candidate_histogram,
                        request_bytes,
                        checked_multiply_u64(
                            count,
                            occurrence_count,
                            "expanding affine DMA requests across iterations",
                        ),
                        "building the affine DMA candidate histogram",
                    )
                if packed_requests_per_occurrence >= logical_transfers:
                    continue
                static_packable_segment_count = checked_add_u64(
                    static_packable_segment_count,
                    1,
                    "counting packable affine DMA segments",
                )
                if strategy == "full":
                    static_full_linear_segment_count += 1
                elif strategy == "piece":
                    static_piece_linear_segment_count += 1
                elif strategy == "repeat":
                    static_repeat_linear_segment_count += 1
                dynamic_logical_request_count = checked_add_u64(
                    dynamic_logical_request_count,
                    checked_multiply_u64(
                        logical_transfers,
                        occurrence_count,
                        "counting packable affine DMA logical requests",
                    ),
                    "counting packable affine DMA logical requests",
                )
                dynamic_packed_request_count = checked_add_u64(
                    dynamic_packed_request_count,
                    checked_multiply_u64(
                        packed_requests_per_occurrence,
                        occurrence_count,
                        "counting packed affine DMA requests",
                    ),
                    "counting packed affine DMA requests",
                )
                maximum_logical_transfers_per_request = max(
                    maximum_logical_transfers_per_request,
                    candidate_width,
                )

        predicted_request_count = 0
        predicted_byte_count = 0
        for request_bytes, count in candidate_histogram.items():
            predicted_request_count = checked_add_u64(
                predicted_request_count,
                count,
                f"reconciling {direction_name} affine-run requests",
            )
            predicted_byte_count = checked_add_u64(
                predicted_byte_count,
                checked_multiply_u64(
                    request_bytes,
                    count,
                    f"reconciling {direction_name} affine-run bytes",
                ),
                f"reconciling {direction_name} affine-run bytes",
            )
        current_requests = current_accounting[direction_name][
            "physical_request_count"
        ]
        current_bytes = current_accounting[direction_name]["physical_byte_count"]
        if predicted_byte_count != current_bytes:
            raise AuditError(
                f"{direction_name} affine-segment run packing changed byte accounting"
            )
        if predicted_request_count > current_requests:
            raise AuditError(
                f"{direction_name} affine-segment run packing increased requests"
            )
        results[direction_name] = {
            "static_packable_segment_count": static_packable_segment_count,
            "static_full_linear_segment_count": static_full_linear_segment_count,
            "static_piece_linear_segment_count": static_piece_linear_segment_count,
            "static_repeat_linear_segment_count": static_repeat_linear_segment_count,
            "dynamic_logical_request_count": dynamic_logical_request_count,
            "dynamic_packed_request_count": dynamic_packed_request_count,
            "maximum_logical_transfers_per_request": (
                maximum_logical_transfers_per_request
            ),
            "predicted_physical_request_count": predicted_request_count,
            "predicted_additional_request_reduction": current_requests
            - predicted_request_count,
            "candidate_request_size_histogram": candidate_histogram,
        }
    return results


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def matching_end(text: str, start: int, opener: str, closer: str) -> int:
    if start >= len(text) or text[start] != opener:
        raise AuditError(f"expected {opener!r} at byte {start}")
    depth = 0
    quoted = False
    escaped = False
    for index in range(start, len(text)):
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
        elif character == opener:
            depth += 1
        elif character == closer:
            depth -= 1
            if depth == 0:
                return index
    raise AuditError(f"unterminated {opener}{closer} attribute at byte {start}")


def attribute_body(text: str, name: str, opener: str, closer: str) -> str:
    matches = list(re.finditer(re.escape(name) + r"\s*=", text))
    if len(matches) != 1:
        raise AuditError(
            f"expected exactly one {name} attribute, found {len(matches)}"
        )
    start = matches[0].end()
    while start < len(text) and text[start].isspace():
        start += 1
    if start >= len(text) or text[start] != opener:
        raise AuditError(f"{name} must be a {opener}{closer} attribute")
    end = matching_end(text, start, opener, closer)
    return text[start + 1 : end]


def split_top_level(text: str) -> list[str]:
    entries: list[str] = []
    start = 0
    stack: list[str] = []
    closing = {"(": ")", "[": "]", "{": "}", "<": ">"}
    quoted = False
    escaped = False
    for index, character in enumerate(text):
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
        elif character in closing:
            stack.append(closing[character])
        elif character in closing.values():
            if not stack or stack.pop() != character:
                raise AuditError("materialization attribute has mismatched delimiters")
        elif character == "," and not stack:
            entries.append(text[start:index].strip())
            start = index + 1
    if quoted or stack:
        raise AuditError("materialization attribute has unterminated syntax")
    final = text[start:].strip()
    if final:
        entries.append(final)
    elif entries:
        raise AuditError("materialization attribute has a trailing comma")
    return entries


def parse_audit(text: str) -> tuple[dict[str, int], bool, bool]:
    body = attribute_body(text, AUDIT_ATTRIBUTE, "{", "}")
    values: dict[str, int] = {}
    for entry in split_top_level(body):
        match = INTEGER_FIELD.fullmatch(entry)
        if match is None:
            raise AuditError(f"malformed materialization audit field: {entry!r}")
        name, value = match.groups()
        if name in values:
            raise AuditError(f"duplicate materialization audit field: {name}")
        values[name] = int(value)
    missing = sorted(set(AUDIT_FIELDS) - set(values))
    unexpected = sorted(set(values) - set(KNOWN_AUDIT_FIELDS))
    if missing or unexpected:
        raise AuditError(
            "materialization audit fields disagree with schema version 1: "
            f"missing={missing}, unexpected={unexpected}"
        )
    phase4_present = set(values).intersection(PHASE4_LOCAL_COUNTERS)
    if phase4_present and phase4_present != set(PHASE4_LOCAL_COUNTERS):
        raise AuditError(
            "Phase 4 materialization accounting fields must be all-or-none: "
            f"missing={sorted(set(PHASE4_LOCAL_COUNTERS) - phase4_present)}"
        )
    if phase4_present:
        missing_phase4_base = sorted(set(OPTIONAL_LOCAL_COUNTERS) - set(values))
        if missing_phase4_base:
            raise AuditError(
                "Phase 4 materialization accounting is missing retained-owner "
                f"base fields: {missing_phase4_base}"
            )
    phase5_present = set(values).intersection(PHASE5_LOCAL_COUNTERS)
    if phase5_present and phase5_present != set(PHASE5_LOCAL_COUNTERS):
        raise AuditError(
            "Phase 5 materialization accounting fields must be all-or-none: "
            f"missing={sorted(set(PHASE5_LOCAL_COUNTERS) - phase5_present)}"
        )
    if phase5_present and not phase4_present:
        raise AuditError(
            "Phase 5 direct-forward accounting requires the complete Phase 4 "
            "physical accounting contract"
        )
    if values["schema_version"] != 1:
        raise AuditError("materialization audit schema_version must equal 1")
    for name in (
        OPTIONAL_GLOBAL_COUNTERS
        + OPTIONAL_LOCAL_COUNTERS
        + PHASE4_LOCAL_COUNTERS
        + PHASE5_LOCAL_COUNTERS
    ):
        values.setdefault(name, 0)
    for name in KNOWN_AUDIT_FIELDS:
        if name != "schema_version" and values[name] < 0:
            raise AuditError(f"materialization audit field {name} is negative")
    if values["epoch_count"] == 0:
        raise AuditError("materialization audit epoch_count must be positive")
    retention_activity = any(values[name] != 0 for name in OPTIONAL_LOCAL_COUNTERS)
    if retention_activity and not phase4_present:
        raise AuditError(
            "retained-owner activity requires the complete Phase 4 accounting contract"
        )
    return values, bool(phase4_present), bool(phase5_present)


def record_fields(
    entry: str, prefix: str, ordinal: int, required: tuple[str, ...]
) -> dict[str, int]:
    if not entry.startswith(prefix) or not entry.endswith(">"):
        raise AuditError(
            f"materialized DMA table entry {ordinal} has the wrong attribute type"
        )
    body = entry[len(prefix) : -1]
    values: dict[str, int] = {}
    for field in split_top_level(body):
        match = INTEGER_FIELD.fullmatch(field)
        if match is None:
            raise AuditError(
                f"materialized DMA table entry {ordinal} has malformed field {field!r}"
            )
        name, value = match.groups()
        if name in values:
            raise AuditError(
                f"materialized DMA table entry {ordinal} repeats field {name}"
            )
        values[name] = int(value)
    missing = sorted(set(required) - set(values))
    if missing:
        raise AuditError(
            f"materialized DMA table entry {ordinal} is missing fields {missing}"
        )
    invalid = sorted(
        name
        for name in required
        if values[name] < 0 or values[name] > (1 << 63) - 1
    )
    if invalid:
        raise AuditError(
            f"materialized DMA table entry {ordinal} has required fields "
            f"outside nonnegative i64: {invalid}"
        )
    return values


def descriptor_accounting(
    text: str, maximum_frame_bytes: int
) -> dict[str, Any]:
    descriptor_entries = split_top_level(
        attribute_body(text, DESCRIPTOR_ATTRIBUTE, "[", "]")
    )
    segment_entries = split_top_level(
        attribute_body(text, SEGMENT_ATTRIBUTE, "[", "]")
    )
    descriptors = [
        record_fields(
            entry,
            "#sculptor.materialized_dma_descriptor<",
            ordinal,
            (
                "id",
                "flags",
                "operationId",
                "epochId",
                "workUnitId",
                "tensorId",
                "portNumber",
                "loopId",
                "direction",
                "templateKind",
                "iterationBegin",
                "iterationEnd",
                "iterationStep",
                "scratchpadRingBase",
                "scratchpadSlotStride",
                "ringSlots",
                "segmentOffset",
                "segmentCount",
                "bytesPerIteration",
            ),
        )
        for ordinal, entry in enumerate(descriptor_entries)
    ]
    segments = [
        record_fields(
            entry,
            "#sculptor.materialized_dma_segment<",
            ordinal,
            (
                "id",
                "descriptorId",
                "relationId",
                "relationPieceOrdinal",
                "globalResourceId",
                "byteSize",
                "repeatCount",
                "pieceCount",
                "globalByteOffset",
                "globalIterationStride",
                "scratchpadByteOffset",
                "globalRepeatStride",
                "scratchpadRepeatStride",
                "globalPieceStride",
                "scratchpadPieceStride",
                "scratchpadIterationStride",
            ),
        )
        for ordinal, entry in enumerate(segment_entries)
    ]

    accounting: dict[str, Any] = {
        "input": {
            "descriptor_count": 0,
            "logical_bytes": 0,
            "logical_transfer_count": 0,
            "physical_byte_count": 0,
            "physical_request_count": 0,
            "physical_request_size_histogram": {},
        },
        "output": {
            "descriptor_count": 0,
            "logical_bytes": 0,
            "logical_transfer_count": 0,
            "physical_byte_count": 0,
            "physical_request_count": 0,
            "physical_request_size_histogram": {},
        },
        "maximum_physical_segment_bytes": 0,
    }
    expected_segment_offset = 0
    compact_leader_flag = 1 << 4
    compact_follower_flag = 1 << 5
    compact_flags = compact_leader_flag | compact_follower_flag

    def validate_compact_pair(leader_index: int, follower_index: int) -> None:
        leader = descriptors[leader_index]
        follower = descriptors[follower_index]
        if (
            follower_index != leader_index + 1
            or follower["id"] != leader["id"] + 1
            or leader["direction"] != 0
            or follower["direction"] != 0
            or (leader["flags"] & compact_flags) != compact_leader_flag
            or (follower["flags"] & compact_flags) != compact_follower_flag
            or (leader["flags"] & ~compact_flags)
            != (follower["flags"] & ~compact_flags)
            or any(
                leader[name] != follower[name]
                for name in (
                    "operationId",
                    "epochId",
                    "workUnitId",
                    "tensorId",
                    "loopId",
                    "templateKind",
                    "iterationBegin",
                    "iterationEnd",
                    "iterationStep",
                    "scratchpadSlotStride",
                    "ringSlots",
                )
            )
            or follower["portNumber"] != leader["portNumber"] + 1
            or leader["segmentCount"] != 1
            or follower["segmentCount"] != 1
            or follower["segmentOffset"] != leader["segmentOffset"] + 1
            or leader["segmentOffset"] >= len(segments)
            or follower["segmentOffset"] >= len(segments)
            or not 0 < leader["bytesPerIteration"] <= maximum_frame_bytes
            or not 0 < follower["bytesPerIteration"]
            <= maximum_frame_bytes - leader["bytesPerIteration"]
        ):
            raise AuditError(
                f"materialized DMA descriptors {leader_index}/{follower_index} "
                "do not form one canonical compact input pair"
            )
        left = segments[leader["segmentOffset"]]
        right = segments[follower["segmentOffset"]]
        if (
            left["descriptorId"] != leader["id"]
            or right["descriptorId"] != follower["id"]
            or left["repeatCount"] != 1
            or right["repeatCount"] != 1
            or left["pieceCount"] != 1
            or right["pieceCount"] != 1
            or left["byteSize"] != leader["bytesPerIteration"]
            or right["byteSize"] != follower["bytesPerIteration"]
            or any(
                left[name] != right[name]
                for name in (
                    "relationId",
                    "relationPieceOrdinal",
                    "globalResourceId",
                    "globalIterationStride",
                    "scratchpadIterationStride",
                    "globalRepeatStride",
                    "scratchpadRepeatStride",
                    "globalPieceStride",
                    "scratchpadPieceStride",
                )
            )
            or left["globalByteOffset"] + left["byteSize"]
            != right["globalByteOffset"]
            or leader["scratchpadRingBase"]
            + left["scratchpadByteOffset"]
            + left["byteSize"]
            != follower["scratchpadRingBase"]
            + right["scratchpadByteOffset"]
        ):
            raise AuditError(
                f"materialized DMA descriptors {leader_index}/{follower_index} "
                "have a noncanonical compact segment join"
            )

    for ordinal, descriptor in enumerate(descriptors):
        if descriptor["id"] != ordinal:
            raise AuditError(
                "materialized DMA descriptor IDs must be dense and ordered: "
                f"entry={ordinal}, id={descriptor['id']}"
            )
        direction_value = descriptor["direction"]
        if direction_value == 0:
            direction = "input"
        elif direction_value == 1:
            direction = "output"
        else:
            raise AuditError(
                f"materialized DMA descriptor {ordinal} has invalid direction "
                f"{direction_value}"
            )
        begin = descriptor["iterationBegin"]
        end = descriptor["iterationEnd"]
        step = descriptor["iterationStep"]
        if begin < 0 or end <= begin or step <= 0:
            raise AuditError(
                f"materialized DMA descriptor {ordinal} has an empty or invalid "
                "iteration interval"
            )
        occurrence_count = 1 + (end - 1 - begin) // step
        segment_offset = descriptor["segmentOffset"]
        segment_count = descriptor["segmentCount"]
        if segment_offset != expected_segment_offset or segment_count <= 0:
            raise AuditError(
                f"materialized DMA descriptor {ordinal} does not own the next "
                "non-empty segment slice"
            )
        segment_end = segment_offset + segment_count
        if segment_end > len(segments):
            raise AuditError(
                f"materialized DMA descriptor {ordinal} segment slice is out of range"
            )

        bytes_per_occurrence = 0
        transfers_per_occurrence = 0
        requests_by_size_per_occurrence: dict[int, int] = {}
        for segment_index in range(segment_offset, segment_end):
            segment = segments[segment_index]
            if segment["id"] != segment_index:
                raise AuditError(
                    "materialized DMA segment IDs must be dense and ordered: "
                    f"entry={segment_index}, id={segment['id']}"
                )
            if segment["descriptorId"] != ordinal:
                raise AuditError(
                    f"materialized DMA segment {segment_index} names descriptor "
                    f"{segment['descriptorId']}, expected {ordinal}"
                )
            byte_size = segment["byteSize"]
            repeat_count = segment["repeatCount"]
            piece_count = segment["pieceCount"]
            if not 0 < byte_size <= maximum_frame_bytes:
                raise AuditError(
                    f"materialized DMA segment {segment_index} byteSize must be in "
                    f"[1, {maximum_frame_bytes}]"
                )
            if repeat_count <= 0 or piece_count <= 0:
                raise AuditError(
                    f"materialized DMA segment {segment_index} has an empty repeat/piece "
                    "domain"
                )
            transfer_count = checked_multiply_u64(
                repeat_count,
                piece_count,
                f"counting requests for materialized DMA segment {segment_index}",
            )
            transfers_per_occurrence = checked_add_u64(
                transfers_per_occurrence,
                transfer_count,
                f"counting transfers for materialized DMA descriptor {ordinal}",
            )
            bytes_per_occurrence = checked_add_u64(
                bytes_per_occurrence,
                checked_multiply_u64(
                    byte_size,
                    transfer_count,
                    f"counting bytes for materialized DMA segment {segment_index}",
                ),
                f"counting bytes for materialized DMA descriptor {ordinal}",
            )
            add_histogram_count(
                requests_by_size_per_occurrence,
                byte_size,
                transfer_count,
                f"counting requests for materialized DMA descriptor {ordinal}",
            )
            accounting["maximum_physical_segment_bytes"] = max(
                accounting["maximum_physical_segment_bytes"], byte_size
            )
        if bytes_per_occurrence != descriptor["bytesPerIteration"]:
            raise AuditError(
                f"materialized DMA descriptor {ordinal} byte accounting disagrees: "
                f"segments={bytes_per_occurrence}, "
                f"descriptor={descriptor['bytesPerIteration']}"
            )
        direction_accounting = accounting[direction]
        direction_accounting["descriptor_count"] = checked_add_u64(
            direction_accounting["descriptor_count"],
            1,
            f"counting {direction} DMA descriptors",
        )
        descriptor_logical_bytes = checked_multiply_u64(
            occurrence_count,
            bytes_per_occurrence,
            f"counting logical bytes for materialized DMA descriptor {ordinal}",
        )
        descriptor_logical_transfers = checked_multiply_u64(
            occurrence_count,
            transfers_per_occurrence,
            f"counting logical transfers for materialized DMA descriptor {ordinal}",
        )
        direction_accounting["logical_bytes"] = checked_add_u64(
            direction_accounting["logical_bytes"],
            descriptor_logical_bytes,
            f"accumulating {direction} logical bytes",
        )
        direction_accounting["logical_transfer_count"] = checked_add_u64(
            direction_accounting["logical_transfer_count"],
            descriptor_logical_transfers,
            f"accumulating {direction} logical transfers",
        )
        direction_accounting["physical_byte_count"] = checked_add_u64(
            direction_accounting["physical_byte_count"],
            descriptor_logical_bytes,
            f"accumulating {direction} physical bytes",
        )
        descriptor_compact_flags = descriptor["flags"] & compact_flags
        if direction != "input" and descriptor_compact_flags:
            raise AuditError(
                f"materialized output DMA descriptor {ordinal} carries compact-input flags"
            )
        if descriptor_compact_flags == compact_flags:
            raise AuditError(
                f"materialized DMA descriptor {ordinal} is both compact leader and follower"
            )
        if descriptor_compact_flags == compact_follower_flag:
            leader = descriptors[ordinal - 1] if ordinal != 0 else None
            if leader is None or (
                leader["flags"] & compact_flags
            ) != compact_leader_flag:
                raise AuditError(
                    f"materialized DMA descriptor {ordinal} has no adjacent compact leader"
                )
            validate_compact_pair(ordinal - 1, ordinal)
            if transfers_per_occurrence != 1:
                raise AuditError(
                    f"compact follower descriptor {ordinal} does not represent one request"
                )
        else:
            if descriptor_compact_flags == compact_leader_flag:
                if ordinal + 1 >= len(descriptors) or (
                    descriptors[ordinal + 1]["flags"] & compact_flags
                ) != compact_follower_flag:
                    raise AuditError(
                        f"materialized DMA descriptor {ordinal} has no adjacent compact follower"
                    )
                validate_compact_pair(ordinal, ordinal + 1)
                if transfers_per_occurrence != 1:
                    raise AuditError(
                        f"compact leader descriptor {ordinal} does not represent one request"
                    )
                request_byte_size = checked_add_u64(
                    descriptor["bytesPerIteration"],
                    descriptors[ordinal + 1]["bytesPerIteration"],
                    f"combining compact DMA descriptor {ordinal}",
                )
                request_count = occurrence_count
                request_histogram = {request_byte_size: request_count}
            else:
                request_count = checked_multiply_u64(
                    occurrence_count,
                    transfers_per_occurrence,
                    f"counting requests for materialized DMA descriptor {ordinal}",
                )
                request_histogram = {
                    byte_size: checked_multiply_u64(
                        occurrence_count,
                        count,
                        f"expanding request-size histogram for descriptor {ordinal}",
                    )
                    for byte_size, count in requests_by_size_per_occurrence.items()
                }
            direction_accounting["physical_request_count"] = checked_add_u64(
                direction_accounting["physical_request_count"],
                request_count,
                f"accumulating {direction} physical requests",
            )
            for byte_size, count in request_histogram.items():
                add_histogram_count(
                    direction_accounting["physical_request_size_histogram"],
                    byte_size,
                    count,
                    f"accumulating {direction} request-size histogram",
                )
        expected_segment_offset = segment_end

    if expected_segment_offset != len(segments):
        raise AuditError(
            "materialized DMA descriptors do not partition the complete segment table"
        )
    for direction in ("input", "output"):
        direction_accounting = accounting[direction]
        histogram = direction_accounting["physical_request_size_histogram"]
        histogram_requests = 0
        histogram_bytes = 0
        for byte_size, count in histogram.items():
            histogram_requests = checked_add_u64(
                histogram_requests,
                count,
                f"reconciling {direction} histogram requests",
            )
            histogram_bytes = checked_add_u64(
                histogram_bytes,
                checked_multiply_u64(
                    byte_size,
                    count,
                    f"reconciling {direction} histogram bytes",
                ),
                f"reconciling {direction} histogram bytes",
            )
        if histogram_requests != direction_accounting["physical_request_count"]:
            raise AuditError(
                f"{direction} request-size histogram count disagrees with physical requests"
            )
        if histogram_bytes != direction_accounting["physical_byte_count"]:
            raise AuditError(
                f"{direction} request-size histogram bytes disagree with physical bytes"
            )
    accounting["maximal_input_dma_run_audit"] = maximal_input_dma_run_audit(
        descriptors,
        segments,
        maximum_frame_bytes,
        accounting["input"]["physical_request_count"],
        accounting["input"]["physical_byte_count"],
    )
    accounting["affine_segment_run_audit"] = affine_segment_run_audit(
        descriptors,
        segments,
        maximum_frame_bytes,
        accounting,
    )
    return accounting


def validate_retention_accounting(
    core: int,
    audit: dict[str, int],
    dma: dict[str, Any],
    phase4_contract: bool,
    maximum_frame_bytes: int,
) -> None:
    retained_count = audit["retained_local_owner_alias_count"]
    hybrid_count = audit["retained_local_hybrid_fanout_count"]
    if hybrid_count > retained_count:
        raise AuditError(
            f"active core {core} hybrid fanout count exceeds retained owners"
        )

    elided: dict[str, dict[str, int]] = {}
    for direction in ("input", "output"):
        direction_elided = {
            "descriptor_count": audit[
                f"elided_retained_{direction}_descriptor_count"
            ],
            "logical_bytes": audit[f"elided_retained_{direction}_logical_bytes"],
            "logical_transfer_count": audit[
                f"elided_retained_{direction}_logical_transfer_count"
            ],
        }
        populated = [value != 0 for value in direction_elided.values()]
        if any(populated) and not all(populated):
            raise AuditError(
                f"active core {core} has an incomplete {direction} elision triplet"
            )
        elided[direction] = direction_elided

    if retained_count == 0 and (
        any(value != 0 for direction in elided.values() for value in direction.values())
        or hybrid_count != 0
    ):
        raise AuditError(
            f"active core {core} reports elision without a retained owner"
        )
    if retained_count != 0 and elided["input"]["descriptor_count"] == 0:
        raise AuditError(
            f"active core {core} retained owners did not elide any input fill"
        )
    if (
        elided["output"]["descriptor_count"] != 0
        and elided["input"]["descriptor_count"] == 0
    ):
        raise AuditError(
            f"active core {core} elides output spills without any retained input fill"
        )
    if hybrid_count != 0 and dma["output"]["descriptor_count"] == 0:
        raise AuditError(
            f"active core {core} hybrid fanout does not retain a global output spill"
        )

    if not phase4_contract:
        return

    direct_input = {
        "descriptor_count": audit[
            "direct_forward_elided_input_descriptor_count"
        ],
        "logical_bytes": audit["direct_forward_elided_input_logical_bytes"],
        "logical_transfer_count": audit[
            "direct_forward_elided_input_logical_transfer_count"
        ],
        "physical_byte_count": audit[
            "direct_forward_elided_input_physical_byte_count"
        ],
        "physical_request_count": audit[
            "direct_forward_elided_input_physical_request_count"
        ],
    }
    direct_output = {
        "descriptor_count": audit[
            "direct_forward_elided_output_descriptor_count"
        ],
        "logical_bytes": audit["direct_forward_elided_output_logical_bytes"],
        "logical_transfer_count": audit[
            "direct_forward_elided_output_logical_transfer_count"
        ],
        "physical_byte_count": audit[
            "direct_forward_elided_output_physical_byte_count"
        ],
        "physical_request_count": audit[
            "direct_forward_elided_output_physical_request_count"
        ],
    }
    selected_direct_routes = audit["direct_forward_selected_route_count"]
    for direction, direct in (("input", direct_input), ("output", direct_output)):
        if direct["descriptor_count"] == 0 and any(
            value != 0 for value in direct.values()
        ):
            raise AuditError(
                f"active core {core} reports direct {direction} accounting without "
                "an elided descriptor"
            )
        if direct["descriptor_count"] != 0 and (
            selected_direct_routes == 0
            or direct["logical_bytes"] == 0
            or direct["logical_transfer_count"] == 0
        ):
            raise AuditError(
                f"active core {core} direct {direction} elision has no selected "
                "route or logical work"
            )
        if direct["physical_byte_count"] > direct["logical_bytes"]:
            raise AuditError(
                f"active core {core} direct {direction} physical byte benefit "
                "exceeds its logical work"
            )
        if direct["physical_request_count"] > direct[
            "logical_transfer_count"
        ]:
            raise AuditError(
                f"active core {core} direct {direction} physical request "
                "benefit exceeds its logical transfers"
            )

    fallback_reasons = (
        audit["retained_local_mixed_lattice_fallback_component_count"]
        + audit["retained_local_epoch_closure_fallback_component_count"]
        + audit["retained_local_capacity_fallback_count"]
    )
    if audit["retained_local_fallback_component_count"] != fallback_reasons:
        raise AuditError(
            f"active core {core} retained fallback reasons are not exhaustive"
        )
    if audit["retained_local_eligible_component_count"] != (
        audit["retained_local_selected_component_count"] + fallback_reasons
    ):
        raise AuditError(
            f"active core {core} retained eligible components are not conserved"
        )
    if audit["retained_local_selected_component_count"] != audit[
        "retained_local_epoch_component_count"
    ]:
        raise AuditError(
            f"active core {core} selected and execution component counts disagree"
        )
    if (audit["retained_local_selected_component_count"] == 0) != (
        audit["retained_local_route_count"] == 0
    ):
        raise AuditError(
            f"active core {core} selected components and retained routes disagree"
        )
    if audit["retained_local_owner_alias_count"] != audit[
        "retained_local_route_count"
    ]:
        raise AuditError(
            f"active core {core} retained alias and route counts disagree"
        )
    if audit["retained_local_owner_count"] > audit["retained_local_route_count"]:
        raise AuditError(
            f"active core {core} retained owner count exceeds its route count"
        )
    if audit["retained_local_policy_enabled_tile_count"] not in (0, 1):
        raise AuditError(
            f"active core {core} retained-local policy marker is not boolean"
        )
    if audit["retained_local_policy_enabled_tile_count"] == 0 and any(
        audit[name] != 0
        for name in (
            "retained_local_eligible_component_count",
            "retained_local_selected_component_count",
            "retained_local_fallback_component_count",
            "retained_local_mixed_lattice_fallback_component_count",
            "retained_local_epoch_closure_fallback_component_count",
            "retained_local_capacity_fallback_count",
            "retained_local_epoch_component_count",
            "retained_local_epoch_coalesced_kernel_count",
            "retained_local_owner_count",
            "retained_local_route_count",
            "retained_local_owner_alias_count",
            "retained_local_logical_bytes",
            "retained_local_logical_transfer_count",
            "elided_retained_input_descriptor_count",
            "elided_retained_output_descriptor_count",
            "elided_retained_input_logical_bytes",
            "elided_retained_output_logical_bytes",
            "elided_retained_input_logical_transfer_count",
            "elided_retained_output_logical_transfer_count",
            "elided_retained_input_physical_request_count",
            "elided_retained_output_physical_request_count",
            "elided_retained_input_physical_byte_count",
            "elided_retained_output_physical_byte_count",
        )
    ):
        raise AuditError(
            f"active core {core} disabled retained-local policy reports retention activity"
        )
    # Route families and descriptor families use different compact domains.
    # Multiple clipped owner-alias routes may jointly replace one aggregate
    # materialized input descriptor, so only nonempty coverage is required
    # here.  Finalize independently proves the exact descriptor-table delta.
    if audit["elided_retained_input_logical_bytes"] != audit[
        "elided_retained_input_physical_byte_count"
    ]:
        raise AuditError(
            f"active core {core} input physical byte certificate disagrees"
        )
    if audit["retained_local_logical_bytes"] != audit[
        "elided_retained_input_physical_byte_count"
    ]:
        raise AuditError(
            f"active core {core} forwarded logical bytes disagree with elided fills"
        )
    if (audit["retained_local_logical_transfer_count"] == 0) != (
        audit["retained_local_route_count"] == 0
    ):
        raise AuditError(
            f"active core {core} forwarded logical transfers disagree with retained routes"
        )
    for unit, legacy_name, physical_name in (
        (
            "bytes",
            "elided_retained_output_logical_bytes",
            "elided_retained_output_physical_byte_count",
        ),
        (
            "requests",
            "elided_retained_output_logical_transfer_count",
            "elided_retained_output_physical_request_count",
        ),
    ):
        if audit[legacy_name] != audit[physical_name]:
            raise AuditError(
                f"active core {core} output {unit} physical certificate disagrees"
            )
    for direction in ("input", "output"):
        for metric in ("request_count", "byte_count"):
            audited_remaining = audit[
                f"materialized_{direction}_physical_{metric}"
            ]
            table_remaining = dma[direction][f"physical_{metric}"]
            if audited_remaining != table_remaining:
                raise AuditError(
                    f"active core {core} {direction} physical {metric} "
                    f"table/audit mismatch: table={table_remaining}, "
                    f"audit={audited_remaining}"
                )
            before = audit[
                f"retained_{direction}_physical_{metric}_before_elision"
            ]
            elided_physical = audit[
                f"elided_retained_{direction}_physical_{metric}"
            ]
            direct_physical = (
                direct_input if direction == "input" else direct_output
            )[f"physical_{metric}"]
            if before != table_remaining + elided_physical + direct_physical:
                raise AuditError(
                    f"active core {core} {direction} physical {metric} is not "
                    f"conserved: before={before}, remaining={table_remaining}, "
                    f"retained_elided={elided_physical}, "
                    f"direct_elided={direct_physical}"
                )
        for stage, request_name, byte_name in (
            (
                "remaining",
                f"materialized_{direction}_physical_request_count",
                f"materialized_{direction}_physical_byte_count",
            ),
            (
                "before elision",
                f"retained_{direction}_physical_request_count_before_elision",
                f"retained_{direction}_physical_byte_count_before_elision",
            ),
            (
                "elided",
                f"elided_retained_{direction}_physical_request_count",
                f"elided_retained_{direction}_physical_byte_count",
            ),
        ):
            requests = audit[request_name]
            physical_bytes = audit[byte_name]
            # A differential request count may be zero while bytes are saved:
            # removing one half of a compact pair leaves one physical request
            # before and after, but reduces its payload.  Absolute before and
            # remaining workloads still require one nonempty bounded payload
            # per request.
            if stage == "elided":
                if requests != 0 and physical_bytes == 0:
                    raise AuditError(
                        f"active core {core} {direction} {stage} physical "
                        "requests have no removed payload bytes"
                    )
                continue
            if (requests == 0) != (physical_bytes == 0):
                raise AuditError(
                    f"active core {core} {direction} {stage} physical requests "
                    "and bytes disagree on empty work"
                )
            if physical_bytes > requests * maximum_frame_bytes:
                raise AuditError(
                    f"active core {core} {direction} {stage} physical bytes "
                    "exceed the configured physical-frame envelope"
                )
    if audit["preserved_hybrid_spill_descriptor_count"] < hybrid_count:
        raise AuditError(
            f"active core {core} hybrid spill descriptors do not cover fanout"
        )
    if hybrid_count == 0 and audit["preserved_hybrid_spill_descriptor_count"] != 0:
        raise AuditError(
            f"active core {core} reports a hybrid spill without hybrid fanout"
        )
    if audit["preserved_hybrid_spill_descriptor_count"] > dma["output"][
        "descriptor_count"
    ]:
        raise AuditError(
            f"active core {core} hybrid spill descriptors exceed physical outputs"
        )
    if audit["direct_forward_preserved_hybrid_spill_descriptor_count"] > dma[
        "output"
    ]["descriptor_count"]:
        raise AuditError(
            f"active core {core} direct hybrid spills exceed physical outputs"
        )
    if audit["preserved_external_spill_descriptor_count"] > dma["output"][
        "descriptor_count"
    ]:
        raise AuditError(
            f"active core {core} external spill descriptors exceed physical outputs"
        )
    capacity = audit["capacity_bytes"]
    required = audit["final_required_local_bytes"]
    headroom = audit["capacity_headroom_bytes"]
    if capacity <= 0 or capacity > 2_097_152 or required + headroom != capacity:
        raise AuditError(
            f"active core {core} final SPM capacity accounting is invalid"
        )
    for direction in ("input", "output"):
        remaining = dma[direction]
        for metric in (
            "descriptor_count",
            "logical_bytes",
            "logical_transfer_count",
        ):
            before = audit[f"retained_{direction}_{metric}_before_elision"]
            direct_elided = (
                direct_input if direction == "input" else direct_output
            )[metric]
            conserved = (
                remaining[metric]
                + elided[direction][metric]
                + direct_elided
            )
            if before != conserved:
                raise AuditError(
                    f"active core {core} {direction} {metric} is not conserved: "
                    f"before={before}, remaining={remaining[metric]}, "
                    f"retained_elided={elided[direction][metric]}, "
                    f"direct_elided={direct_elided}"
                )


def read_active_cores(path: Path) -> list[int]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise AuditError(f"cannot read active-core manifest {path}: {error}") from error
    if not lines:
        raise AuditError("active-core manifest is empty")
    cores: list[int] = []
    for line_number, line in enumerate(lines, start=1):
        value = line.strip()
        if re.fullmatch(r"[0-9]+", value) is None:
            raise AuditError(
                f"active-core manifest line {line_number} is not an unsigned integer"
            )
        cores.append(int(value))
    duplicates = sorted(core for core in set(cores) if cores.count(core) > 1)
    if duplicates:
        raise AuditError(f"active-core manifest contains duplicates: {duplicates}")
    return cores


def finalized_artifacts(core_directory: Path) -> dict[int, Path]:
    artifacts: dict[int, Path] = {}
    for path in core_directory.glob("core-*-finalized.mlir"):
        match = FINALIZED_PATTERN.fullmatch(path.name)
        if match is None:
            continue
        core = int(match.group(1))
        if core in artifacts:
            raise AuditError(
                f"multiple finalized artifacts identify active core {core}: "
                f"{artifacts[core]}, {path}"
            )
        artifacts[core] = path
    return artifacts


def validate(args: argparse.Namespace) -> dict[str, Any]:
    if args.network_size <= 0:
        raise AuditError("network size must be positive")
    if args.global_ram_bytes <= 0:
        raise AuditError("global RAM capacity must be positive")
    if args.maximum_frame_bytes not in {
        4096,
        8192,
        16384,
        32768,
        65536,
        131072,
        262144,
    }:
        raise AuditError(
            "maximum frame bytes must be a supported power of two from "
            "4096 through 262144"
        )

    active_cores = read_active_cores(args.active_core_manifest)
    try:
        deployment = load_deployment_manifest(
            args.deployment_manifest,
            network_size=args.network_size,
            expected_active_tiles=active_cores,
        )
    except (RuntimeError, ValueError) as error:
        raise AuditError(str(error)) from error

    artifacts = finalized_artifacts(args.core_directory)
    expected = set(active_cores)
    actual = set(artifacts)
    if expected != actual:
        raise AuditError(
            "finalized materialization artifacts disagree with active cores: "
            f"missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}"
        )

    common: dict[str, int] | None = None
    total_inputs = total_outputs = 0
    local_totals = {
        name: 0
        for name in (
            OPTIONAL_LOCAL_COUNTERS
            + PHASE4_LOCAL_COUNTERS
            + PHASE5_LOCAL_COUNTERS
        )
    }
    remaining_totals = {
        "remaining_input_logical_bytes": 0,
        "remaining_output_logical_bytes": 0,
        "remaining_input_logical_transfer_count": 0,
        "remaining_output_logical_transfer_count": 0,
        "remaining_input_physical_byte_count": 0,
        "remaining_output_physical_byte_count": 0,
        "remaining_input_physical_request_count": 0,
        "remaining_output_physical_request_count": 0,
    }
    request_size_histograms: dict[str, dict[int, int]] = {
        "input": {},
        "output": {},
    }
    maximal_run_totals = {
        "static_run_count": 0,
        "static_descriptor_count": 0,
        "maximum_run_descriptor_count": 0,
        "maximum_run_bytes": 0,
        "dynamic_packed_request_count": 0,
        "predicted_physical_request_count": 0,
        "predicted_additional_request_reduction": 0,
    }
    maximal_run_length_histogram: dict[int, int] = {}
    maximal_candidate_request_size_histogram: dict[int, int] = {}
    affine_run_sum_fields = (
        "static_packable_segment_count",
        "static_full_linear_segment_count",
        "static_piece_linear_segment_count",
        "static_repeat_linear_segment_count",
        "dynamic_logical_request_count",
        "dynamic_packed_request_count",
        "predicted_physical_request_count",
        "predicted_additional_request_reduction",
    )
    affine_run_totals = {
        direction: {
            **{name: 0 for name in affine_run_sum_fields},
            "maximum_logical_transfers_per_request": 1,
        }
        for direction in ("input", "output")
    }
    affine_candidate_request_size_histograms: dict[str, dict[int, int]] = {
        "input": {},
        "output": {},
    }
    maximum_physical_segment_bytes = 0
    maximum_final_required_local_bytes = 0
    minimum_capacity_headroom_bytes: int | None = None
    phase4_contract_modes: set[bool] = set()
    phase5_contract_modes: set[bool] = set()
    tile_records: list[dict[str, Any]] = []
    for core in sorted(active_cores):
        artifact = artifacts[core]
        try:
            text = artifact.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise AuditError(f"cannot read finalized tile {artifact}: {error}") from error
        audit, phase4_contract, phase5_contract = parse_audit(text)
        phase4_contract_modes.add(phase4_contract)
        phase5_contract_modes.add(phase5_contract)
        global_values = {name: audit[name] for name in GLOBAL_FIELDS}
        if common is None:
            common = global_values
        elif global_values != common:
            differences = sorted(
                name for name in GLOBAL_FIELDS if global_values[name] != common[name]
            )
            raise AuditError(
                f"active core {core} has divergent global materialization fields: "
                + ", ".join(differences)
            )

        dma = descriptor_accounting(text, args.maximum_frame_bytes)
        table_inputs = dma["input"]["descriptor_count"]
        table_outputs = dma["output"]["descriptor_count"]
        audit_inputs = audit["materialized_input_dma_descriptor_count"]
        audit_outputs = audit["materialized_output_dma_descriptor_count"]
        if (table_inputs, table_outputs) != (audit_inputs, audit_outputs):
            raise AuditError(
                f"active core {core} DMA table/audit mismatch: "
                f"table_input={table_inputs}, audit_input={audit_inputs}, "
                f"table_output={table_outputs}, audit_output={audit_outputs}"
            )
        total_inputs = checked_add_u64(
            total_inputs,
            audit_inputs,
            f"aggregating active core {core} input descriptors",
        )
        total_outputs = checked_add_u64(
            total_outputs,
            audit_outputs,
            f"aggregating active core {core} output descriptors",
        )
        for name in (
            OPTIONAL_LOCAL_COUNTERS
            + PHASE4_LOCAL_COUNTERS
            + PHASE5_LOCAL_COUNTERS
        ):
            local_totals[name] = checked_add_u64(
                local_totals[name],
                audit[name],
                f"aggregating active core {core} field {name}",
            )
        validate_retention_accounting(
            core,
            audit,
            dma,
            phase4_contract,
            args.maximum_frame_bytes,
        )
        maximal_run_audit = dma["maximal_input_dma_run_audit"]
        for name in (
            "static_run_count",
            "static_descriptor_count",
            "dynamic_packed_request_count",
            "predicted_physical_request_count",
            "predicted_additional_request_reduction",
        ):
            maximal_run_totals[name] = checked_add_u64(
                maximal_run_totals[name],
                maximal_run_audit[name],
                f"aggregating tile {core} maximal input-DMA field {name}",
            )
        for name in ("maximum_run_descriptor_count", "maximum_run_bytes"):
            maximal_run_totals[name] = max(
                maximal_run_totals[name], maximal_run_audit[name]
            )
        for run_length, count in maximal_run_audit[
            "run_length_histogram"
        ].items():
            add_histogram_count(
                maximal_run_length_histogram,
                run_length,
                count,
                f"aggregating tile {core} maximal input-DMA run lengths",
            )
        for byte_size, count in maximal_run_audit[
            "candidate_request_size_histogram"
        ].items():
            add_histogram_count(
                maximal_candidate_request_size_histogram,
                byte_size,
                count,
                f"aggregating tile {core} maximal input-DMA request sizes",
            )
        affine_run_audit = dma["affine_segment_run_audit"]
        for direction in ("input", "output"):
            for name in affine_run_sum_fields:
                affine_run_totals[direction][name] = checked_add_u64(
                    affine_run_totals[direction][name],
                    affine_run_audit[direction][name],
                    f"aggregating tile {core} {direction} affine-run field {name}",
                )
            affine_run_totals[direction][
                "maximum_logical_transfers_per_request"
            ] = max(
                affine_run_totals[direction][
                    "maximum_logical_transfers_per_request"
                ],
                affine_run_audit[direction][
                    "maximum_logical_transfers_per_request"
                ],
            )
            for byte_size, count in affine_run_audit[direction][
                "candidate_request_size_histogram"
            ].items():
                add_histogram_count(
                    affine_candidate_request_size_histograms[direction],
                    byte_size,
                    count,
                    f"aggregating tile {core} {direction} affine-run request sizes",
                )
        for direction in ("input", "output"):
            for suffix, source_name in (
                ("logical_bytes", "logical_bytes"),
                ("logical_transfer_count", "logical_transfer_count"),
                ("physical_byte_count", "physical_byte_count"),
                ("physical_request_count", "physical_request_count"),
            ):
                counter_name = f"remaining_{direction}_{suffix}"
                remaining_totals[counter_name] = checked_add_u64(
                    remaining_totals[counter_name],
                    dma[direction][source_name],
                    f"aggregating active core {core} field {counter_name}",
                )
            for byte_size, count in dma[direction][
                "physical_request_size_histogram"
            ].items():
                add_histogram_count(
                    request_size_histograms[direction],
                    byte_size,
                    count,
                    f"aggregating tile {core} {direction} request-size histogram",
                )
        maximum_physical_segment_bytes = max(
            maximum_physical_segment_bytes,
            dma["maximum_physical_segment_bytes"],
        )
        maximum_final_required_local_bytes = max(
            maximum_final_required_local_bytes,
            audit["final_required_local_bytes"],
        )
        minimum_capacity_headroom_bytes = (
            audit["capacity_headroom_bytes"]
            if minimum_capacity_headroom_bytes is None
            else min(
                minimum_capacity_headroom_bytes,
                audit["capacity_headroom_bytes"],
            )
        )
        tile_record = {
            "tile_id": core,
            "source": str(artifact.resolve()),
            "sha256": sha256(artifact),
            "materialized_input_dma_descriptor_count": audit_inputs,
            "materialized_output_dma_descriptor_count": audit_outputs,
            "remaining_input_logical_bytes": dma["input"]["logical_bytes"],
            "remaining_output_logical_bytes": dma["output"]["logical_bytes"],
            "remaining_input_logical_transfer_count": dma["input"][
                "logical_transfer_count"
            ],
            "remaining_output_logical_transfer_count": dma["output"][
                "logical_transfer_count"
            ],
            "remaining_input_physical_byte_count": dma["input"][
                "physical_byte_count"
            ],
            "remaining_output_physical_byte_count": dma["output"][
                "physical_byte_count"
            ],
            "remaining_input_physical_request_count": dma["input"][
                "physical_request_count"
            ],
            "remaining_output_physical_request_count": dma["output"][
                "physical_request_count"
            ],
            "maximum_physical_segment_bytes": dma[
                "maximum_physical_segment_bytes"
            ],
            "physical_request_size_histograms": {
                direction: {
                    str(byte_size): count
                    for byte_size, count in sorted(
                        dma[direction]["physical_request_size_histogram"].items()
                    )
                }
                for direction in ("input", "output")
            },
            "maximal_input_dma_run_audit": {
                **{
                    name: maximal_run_audit[name]
                    for name in (
                        "static_run_count",
                        "static_descriptor_count",
                        "maximum_run_descriptor_count",
                        "maximum_run_bytes",
                        "dynamic_packed_request_count",
                        "predicted_physical_request_count",
                        "predicted_additional_request_reduction",
                    )
                },
                "run_length_histogram": {
                    str(run_length): count
                    for run_length, count in sorted(
                        maximal_run_audit["run_length_histogram"].items()
                    )
                },
                "candidate_request_size_histogram": {
                    str(byte_size): count
                    for byte_size, count in sorted(
                        maximal_run_audit[
                            "candidate_request_size_histogram"
                        ].items()
                    )
                },
            },
            "affine_segment_run_audit": {
                direction: {
                    **{
                        name: affine_run_audit[direction][name]
                        for name in (
                            *affine_run_sum_fields,
                            "maximum_logical_transfers_per_request",
                        )
                    },
                    "candidate_request_size_histogram": {
                        str(byte_size): count
                        for byte_size, count in sorted(
                            affine_run_audit[direction][
                                "candidate_request_size_histogram"
                            ].items()
                        )
                    },
                }
                for direction in ("input", "output")
            },
            "phase4_accounting_contract": phase4_contract,
            "phase5_accounting_contract": phase5_contract,
        }
        tile_record.update(
            {
                name: audit[name]
                for name in (
                    OPTIONAL_LOCAL_COUNTERS
                    + PHASE4_LOCAL_COUNTERS
                    + PHASE5_LOCAL_COUNTERS
                )
            }
        )
        tile_records.append(tile_record)

    if common is None:
        raise AuditError("no active finalized tile audits were validated")
    if len(phase4_contract_modes) != 1:
        raise AuditError(
            "active tiles mix legacy and Phase 4 materialization accounting contracts"
        )
    if len(phase5_contract_modes) != 1:
        raise AuditError(
            "active tiles mix pre-Phase-5 and Phase 5 materialization "
            "accounting contracts"
        )
    if common["epoch_count"] != deployment["epoch_count"]:
        raise AuditError(
            "materialization/deployment epoch mismatch: "
            f"audit={common['epoch_count']}, deployment={deployment['epoch_count']}"
        )
    nonzero = {
        name: common[name]
        for name in ZERO_CORRECTNESS_FIELDS
        if common[name] != 0
    }
    if nonzero:
        raise AuditError(
            "materialization correctness counters are nonzero: "
            + ", ".join(f"{name}={value}" for name, value in nonzero.items())
        )
    if common["maximum_live_global_ram_bytes"] > args.global_ram_bytes:
        raise AuditError(
            "materialization live RAM exceeds configured global RAM: "
            f"required={common['maximum_live_global_ram_bytes']}, "
            f"configured={args.global_ram_bytes}"
        )

    counters = {
        name: common[name]
        for name in GLOBAL_FIELDS
        if name != "schema_version"
    }
    counters["materialized_input_dma_descriptor_count"] = total_inputs
    counters["materialized_output_dma_descriptor_count"] = total_outputs
    counters.update(local_totals)
    counters.update(remaining_totals)
    counters["maximum_physical_segment_bytes"] = maximum_physical_segment_bytes
    counters["maximum_final_required_local_bytes"] = (
        maximum_final_required_local_bytes
    )
    counters["minimum_capacity_headroom_bytes"] = (
        0
        if minimum_capacity_headroom_bytes is None
        else minimum_capacity_headroom_bytes
    )
    counters["phase4_accounting_tile_count"] = (
        len(active_cores) if True in phase4_contract_modes else 0
    )
    counters["phase5_accounting_tile_count"] = (
        len(active_cores) if True in phase5_contract_modes else 0
    )
    return {
        "schema": "sculptor.materialization-audit",
        "version": 1,
        "status": "PASS",
        "audit_schema_version": common["schema_version"],
        "active_tile_ids": sorted(active_cores),
        "global_ram_bytes": args.global_ram_bytes,
        "maximum_frame_bytes": args.maximum_frame_bytes,
        "counters": counters,
        "physical_request_size_histograms": {
            direction: {
                str(byte_size): count
                for byte_size, count in sorted(
                    request_size_histograms[direction].items()
                )
            }
            for direction in ("input", "output")
        },
        "maximal_input_dma_run_audit": {
            **maximal_run_totals,
            "run_length_histogram": {
                str(run_length): count
                for run_length, count in sorted(
                    maximal_run_length_histogram.items()
                )
            },
            "candidate_request_size_histogram": {
                str(byte_size): count
                for byte_size, count in sorted(
                    maximal_candidate_request_size_histogram.items()
                )
            },
        },
        "affine_segment_run_audit": {
            direction: {
                **affine_run_totals[direction],
                "candidate_request_size_histogram": {
                    str(byte_size): count
                    for byte_size, count in sorted(
                        affine_candidate_request_size_histograms[
                            direction
                        ].items()
                    )
                },
            }
            for direction in ("input", "output")
        },
        "tiles": tile_records,
        "sources": {
            "active_core_manifest": {
                "path": str(args.active_core_manifest.resolve()),
                "sha256": sha256(args.active_core_manifest),
            },
            "deployment_manifest": {
                "path": str(args.deployment_manifest.resolve()),
                "sha256": sha256(args.deployment_manifest),
            },
        },
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-directory", type=Path, required=True)
    parser.add_argument("--active-core-manifest", type=Path, required=True)
    parser.add_argument("--deployment-manifest", type=Path, required=True)
    parser.add_argument("--network-size", type=int, required=True)
    parser.add_argument("--global-ram-bytes", type=int, required=True)
    parser.add_argument("--maximum-frame-bytes", type=int, default=4096)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        certificate = validate(args)
    except (AuditError, OSError, UnicodeError) as error:
        certificate = {
            "schema": "sculptor.materialization-audit",
            "version": 1,
            "status": "FAIL",
            "active_tile_ids": [],
            "global_ram_bytes": args.global_ram_bytes,
            "maximum_frame_bytes": args.maximum_frame_bytes,
            "counters": {},
            "tiles": [],
            "sources": {
                "active_core_manifest": str(args.active_core_manifest.resolve()),
                "deployment_manifest": str(args.deployment_manifest.resolve()),
                "core_directory": str(args.core_directory.resolve()),
            },
            "errors": [str(error)],
        }
        write_certificate(args.output, certificate)
        print(f"materialization audit failed: {error}", file=sys.stderr)
        return 1

    write_certificate(args.output, certificate)
    counters = certificate["counters"]
    print(
        "materialization audit PASS: "
        f"tiles={len(certificate['active_tile_ids'])} "
        f"epochs={counters['epoch_count']} "
        f"input_dma={counters['materialized_input_dma_descriptor_count']} "
        f"output_dma={counters['materialized_output_dma_descriptor_count']} "
        f"max_live_ram={counters['maximum_live_global_ram_bytes']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
