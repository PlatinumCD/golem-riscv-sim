#!/usr/bin/env python3
"""Collect per-tile Sculptor memory contracts and validate linked heap space."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import sys


FIELD_PATTERN = re.compile(
    r'(\w+) = (?:(?:"([^"]*)")|(-?[0-9]+) : i64|(true|false))'
)
CORE_PATTERN = re.compile(r"core-([0-9]+)-compute-llvm\.mlir$")
HEAP_PROFILE_PATTERN = re.compile(
    r"SCULPTOR_HEAP_PROFILE "
    r"tile=(?P<tile>[0-9]+) "
    r"allocation_count=(?P<allocation_count>[0-9]+) "
    r"current_live_bytes=(?P<current_live_bytes>[0-9]+) "
    r"peak_live_bytes=(?P<peak_live_bytes>[0-9]+) "
    r"failed_allocation_count=(?P<failed_allocation_count>[0-9]+) "
    r"failed_allocation_size=(?P<failed_allocation_size>[0-9]+)"
)
DEFAULT_SCRATCHPAD_CAPACITY_BYTES = 2 * 1024 * 1024

AUDIT_FIELDS = {
    "schema_version",
    "core_id",
    "strict",
    "allocation_count",
    "static_allocation_bytes",
    "approved_local_allocation_count",
    "unplanned_allocation_count",
    "escaping_allocation_count",
    "missing_deallocation_count",
    "routine_lifetime_allocation_count",
    "copy_count",
    "static_copy_bytes",
    "planned_assembly_copy_count",
    "planned_assembly_copy_bytes",
    "planned_boot_staging_copy_count",
    "planned_boot_staging_copy_bytes",
    "unplanned_copy_count",
    "unplanned_full_tensor_copy_count",
    "pure_copy_loop_count",
    "subview_count",
}

VECTOR_FIELDS = {
    "schema_version",
    "vector_bits",
    "copy_count",
    "vectorized_copy_count",
    "vectorized_copy_bytes",
    "masked_tail_copy_count",
    "fallback_copy_count",
    "fallback_copy_bytes",
    "unknown_fallback_bytes_count",
    "scalar_rank_zero_copy_count",
    "non_contiguous_copy_count",
    "route_pack_count",
    "assembly_pack_count",
}


def matching_end(text: str, start: int, opening: str, closing: str) -> int:
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
        elif character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError(f"unterminated {opening}{closing} attribute")


def attribute_span(
    text: str, marker: str, opening: str, closing: str
) -> str:
    marker_index = text.find(marker)
    if marker_index < 0:
        raise ValueError(f"missing MLIR attribute: {marker.rstrip(' =')}")
    start = text.find(opening, marker_index + len(marker))
    if start < 0:
        raise ValueError(f"malformed MLIR attribute: {marker.rstrip(' =')}")
    return text[start : matching_end(text, start, opening, closing)]


def parse_scalars(
    text: str, allowed: set[str] | None = None
) -> dict[str, int | bool | str]:
    values: dict[str, int | bool | str] = {}
    for name, string_value, integer_value, bool_value in FIELD_PATTERN.findall(
        text
    ):
        if allowed is not None and name not in allowed:
            continue
        if string_value:
            values[name] = string_value
        elif integer_value:
            values[name] = int(integer_value)
        else:
            values[name] = bool_value == "true"
    return values


def parse_reports(text: str) -> dict[str, dict[str, int | bool | str]]:
    report_list = attribute_span(
        text, "sculptor.memory.reports = ", "[", "]"
    )
    reports: dict[str, dict[str, int | bool | str]] = {}
    index = 0
    while True:
        start = report_list.find("{", index)
        if start < 0:
            break
        end = matching_end(report_list, start, "{", "}")
        report = parse_scalars(report_list[start:end])
        stage = report.get("stage")
        if isinstance(stage, str):
            reports[stage] = report
        index = end
    return reports


def parse_capacity(text: str) -> dict[str, int | bool | str]:
    capacity = attribute_span(
        text, "sculptor.memory.capacity = ", "<", ">"
    )
    values = parse_scalars(capacity)
    if "tile" not in values or "requiredLocalBytes" not in values:
        raise ValueError("invalid Sculptor tile-memory capacity attribute")
    return values


def parse_dictionary(
    text: str, marker: str, allowed: set[str]
) -> dict[str, int | bool | str]:
    return parse_scalars(attribute_span(text, marker, "{", "}"), allowed)


def parse_optional_dictionary(
    text: str, marker: str, allowed: set[str]
) -> dict[str, int | bool | str]:
    if marker not in text:
        return {}
    return parse_dictionary(text, marker, allowed)


def numeric_summary(
    tiles: dict[str, dict[str, object]], section: str
) -> dict[str, dict[str, int]]:
    identity_fields = {"schema_version", "core_id", "tile"}
    fields = sorted(
        {
            name
            for tile in tiles.values()
            for name, value in dict(tile.get(section, {})).items()
            if isinstance(value, int) and not isinstance(value, bool)
            and name not in identity_fields
        }
    )
    return {
        "totals": {
            name: sum(int(dict(tile.get(section, {})).get(name, 0))
                      for tile in tiles.values())
            for name in fields if name != "vector_bits"
        },
        "maximums": {
            name: max(int(dict(tile.get(section, {})).get(name, 0))
                      for tile in tiles.values())
            for name in fields
        },
    }


def validate_capacity_contract(
    tiles: dict[str, dict[str, object]], scratchpad_capacity_bytes: int
) -> list[str]:
    failures: list[str] = []
    if scratchpad_capacity_bytes <= 0:
        return ["scratchpad capacity must be positive"]
    for core_key, tile_record in sorted(tiles.items(), key=lambda item: int(item[0])):
        core_id = int(core_key)
        capacity = dict(tile_record.get("capacity", {}))
        tile = capacity.get("tile")
        required = capacity.get("requiredLocalBytes")
        complete = capacity.get("complete")
        if not isinstance(tile, int) or isinstance(tile, bool) or tile != core_id:
            failures.append(
                f"tile {core_id} capacity record identifies tile {tile!r}"
            )
        if not isinstance(required, int) or isinstance(required, bool) or required < 0:
            failures.append(
                f"tile {core_id} has invalid requiredLocalBytes {required!r}"
            )
        elif required > scratchpad_capacity_bytes:
            failures.append(
                f"tile {core_id} requires {required} local bytes, exceeding the "
                f"{scratchpad_capacity_bytes}-byte scratchpad"
            )
        if complete is not True:
            failures.append(f"tile {core_id} has an incomplete memory plan")
    return failures


def read_symbols(llvm_nm: Path, elf: Path) -> dict[str, int]:
    result = subprocess.run(
        [str(llvm_nm), "--defined-only", "-n", str(elf)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(f"llvm-nm failed for {elf}:\n{result.stderr}")
    symbols: dict[str, int] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            symbols[fields[-1]] = int(fields[0], 16)
    return symbols


def physical_memory(
    llvm_nm: Path,
    elf: Path,
    tile: dict[str, object],
) -> dict[str, int]:
    symbols = read_symbols(llvm_nm, elf)
    required_symbols = {
        "_start",
        "__heap_start",
        "__heap_end",
        "__stack_bottom",
        "__stack_top",
    }
    missing = sorted(required_symbols - symbols.keys())
    if missing:
        raise ValueError(f"{elf} is missing linker symbols: {', '.join(missing)}")

    heap_start = (symbols["__heap_start"] + 63) & ~63
    heap_end = symbols["__heap_end"] & ~63
    heap_bytes = max(0, heap_end - heap_start)
    capacity = dict(tile["capacity"])
    llvm_report = dict(tile["llvm"])
    audit = dict(tile["audit"])

    payload_bytes = sum(
        int(capacity.get(name, 0))
        for name in (
            "externalBytes",
            "persistentBytes",
            "workspaceBytes",
            "routineTemporaryPeakBytes",
        )
    ) + int(llvm_report.get("runtime_descriptor_bytes", 0))
    allocation_count = (
        12
        + int(llvm_report.get("model_io_record_count", 0))
        + int(audit.get("allocation_count", 0))
    )
    allocator_overhead_bytes = allocation_count * 128 + 64
    estimated_heap_required_bytes = payload_bytes + allocator_overhead_bytes

    return {
        "tile_memory_bytes": symbols["__stack_top"] - symbols["_start"],
        "static_image_bytes": symbols["__heap_start"] - symbols["_start"],
        "stack_bytes": symbols["__stack_top"] - symbols["__stack_bottom"],
        "heap_bytes": heap_bytes,
        "estimated_heap_payload_bytes": payload_bytes,
        "estimated_allocator_overhead_bytes": allocator_overhead_bytes,
        "estimated_heap_required_bytes": estimated_heap_required_bytes,
        "estimated_heap_headroom_bytes": heap_bytes - estimated_heap_required_bytes,
    }


def parse_heap_profiles(path: Path) -> dict[str, dict[str, int]]:
    profiles: dict[str, dict[str, int]] = {}
    for match in HEAP_PROFILE_PATTERN.finditer(
        path.read_text(encoding="utf-8", errors="replace")
    ):
        values = {name: int(value) for name, value in match.groupdict().items()}
        tile = str(values.pop("tile"))
        if tile in profiles:
            raise ValueError(f"simulation log repeats heap profile for tile {tile}")
        profiles[tile] = values
    return profiles


def flatten(tile: dict[str, object]) -> dict[str, int | bool | str]:
    row: dict[str, int | bool | str] = {"core_id": int(tile["core_id"])}
    for section in (
        "capacity",
        "finalized",
        "llvm",
        "audit",
        "vector",
        "physical",
        "runtime_heap",
    ):
        for name, value in dict(tile.get(section, {})).items():
            row[f"{section}_{name}"] = value  # type: ignore[assignment]
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--elf-directory", type=Path)
    parser.add_argument("--llvm-nm", type=Path)
    parser.add_argument("--validate-heap", action="store_true")
    parser.add_argument(
        "--scratchpad-capacity-bytes",
        type=int,
        default=DEFAULT_SCRATCHPAD_CAPACITY_BYTES,
        help="per-tile local-memory capacity (default: 2 MiB)",
    )
    parser.add_argument("--simulation-log", type=Path)
    parser.add_argument("--require-heap-profile", action="store_true")
    parser.add_argument(
        "--reuse-summary",
        action="store_true",
        help="reuse an existing JSON summary when generated MLIR was removed",
    )
    args = parser.parse_args()

    json_path = args.output_directory / "tile-memory-summary.json"
    csv_path = args.output_directory / "tile-memory.csv"
    artifacts = sorted(
        args.core_directory.glob("core-*-compute-llvm.mlir"),
        key=lambda path: int(CORE_PATTERN.search(path.name).group(1))
        if CORE_PATTERN.search(path.name)
        else sys.maxsize,
    )
    if not artifacts and not (args.reuse_summary and json_path.is_file()):
        parser.error(f"no per-core LLVM MLIR artifacts in {args.core_directory}")
    if args.elf_directory is not None and args.llvm_nm is None:
        parser.error("--elf-directory requires --llvm-nm")

    tiles: dict[str, dict[str, object]] = {}
    if not artifacts:
        existing = json.loads(json_path.read_text(encoding="utf-8"))
        tiles = dict(existing.get("tiles", {}))
        if not tiles:
            raise ValueError(f"existing memory summary has no tiles: {json_path}")
    failures: list[str] = []
    for artifact in artifacts:
        match = CORE_PATTERN.search(artifact.name)
        if match is None:
            continue
        core_id = int(match.group(1))
        text = artifact.read_text(encoding="utf-8")
        reports = parse_reports(text)
        if "finalized" not in reports or "llvm" not in reports:
            raise ValueError(f"{artifact} does not contain both memory reports")
        tile: dict[str, object] = {
            "core_id": core_id,
            "artifact": str(artifact),
            "capacity": parse_capacity(text),
            "finalized": reports["finalized"],
            "llvm": reports["llvm"],
            "audit": parse_dictionary(
                text, "sculptor.memory.bufferization_audit = ", AUDIT_FIELDS
            ),
            "vector": parse_optional_dictionary(
                text, "sculptor.memory.vectorized_copy_summary = ", VECTOR_FIELDS
            ),
        }
        tiles[str(core_id)] = tile

    capacity_failures = validate_capacity_contract(
        tiles, args.scratchpad_capacity_bytes
    )
    failures.extend(capacity_failures)

    if args.elf_directory is not None:
        for core_key, tile in tiles.items():
            core_id = int(core_key)
            elf = args.elf_directory / f"tile-{core_id}.elf"
            if not elf.is_file():
                raise ValueError(f"missing active-tile ELF: {elf}")
            tile["physical"] = physical_memory(args.llvm_nm, elf, tile)
            physical = dict(tile["physical"])
            if args.validate_heap and int(
                physical["estimated_heap_headroom_bytes"]
            ) < 0:
                failures.append(
                    f"tile {core_id} needs an estimated "
                    f"{physical['estimated_heap_required_bytes']} heap bytes but "
                    f"the linked ELF provides {physical['heap_bytes']}"
                )
    if args.simulation_log is not None:
        if not args.simulation_log.is_file():
            parser.error(f"missing simulation log: {args.simulation_log}")
        heap_profiles = parse_heap_profiles(args.simulation_log)
        unknown = sorted(heap_profiles.keys() - tiles.keys(), key=int)
        if unknown:
            raise ValueError(
                "heap profiles name inactive tiles: " + ", ".join(unknown)
            )
        for core_id, profile in heap_profiles.items():
            tiles[core_id]["runtime_heap"] = profile
            if "physical" in tiles[core_id]:
                physical = dict(tiles[core_id]["physical"])
                physical["estimated_minus_observed_peak_bytes"] = (
                    int(physical["estimated_heap_required_bytes"])
                    - profile["peak_live_bytes"]
                )
                tiles[core_id]["physical"] = physical
        if args.require_heap_profile:
            missing = sorted(tiles.keys() - heap_profiles.keys(), key=int)
            if missing:
                failures.append(
                    "simulation log has no heap profile for active tiles: "
                    + ", ".join(missing)
                )

    sections = (
        "capacity",
        "finalized",
        "llvm",
        "audit",
        "vector",
        "physical",
        "runtime_heap",
    )
    document = {
        "schema_version": 1,
        "active_tile_count": len(tiles),
        "scratchpad_capacity_bytes": args.scratchpad_capacity_bytes,
        "capacity_gate": {
            "status": "FAIL" if capacity_failures else "PASS",
            "errors": capacity_failures,
        },
        "summaries": {
            section: numeric_summary(tiles, section)
            for section in sections
            if any(section in tile for tile in tiles.values())
        },
        "tiles": tiles,
    }
    args.output_directory.mkdir(parents=True, exist_ok=True)
    json_path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    rows = [flatten(tile) for tile in tiles.values()]
    columns = ["core_id"] + sorted(
        {name for row in rows for name in row if name != "core_id"}
    )
    with csv_path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Sculptor memory reports: {len(tiles)} tiles -> {json_path}")
    if failures:
        for failure in failures:
            print(f"memory validation failed: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
