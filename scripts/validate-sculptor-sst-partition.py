#!/usr/bin/env python3
"""Validate that an SST component partition uses its requested host threads."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


RANK = re.compile(r"^Rank: ([0-9]+)\.([0-9]+) Component List:$")
COMPONENT = re.compile(r"^   ([^ ]+) \(ID=[0-9]+\)$")
TILE = re.compile(r"^tile([0-9]+)$")


def read_active_tiles(path: Path) -> list[int]:
    try:
        tiles = [int(line) for line in path.read_text().splitlines()]
    except ValueError as error:
        raise ValueError(f"invalid active tile list: {error}") from error
    if not tiles or tiles != sorted(set(tiles)) or tiles[0] < 0:
        raise ValueError("active tiles must be nonempty, sorted, and unique")
    return tiles


def validate_partition(
    text: str,
    active_tiles: list[int],
    thread_count: int,
    require_active_tile_per_thread: bool,
) -> dict[str, object]:
    if thread_count <= 0:
        raise ValueError("thread count must be positive")
    if require_active_tile_per_thread and thread_count > len(active_tiles):
        raise ValueError("requested SST threads exceed active tiles")

    components: list[list[str]] = [[] for _ in range(thread_count)]
    current_thread: int | None = None
    seen_sections: set[int] = set()
    for line_number, line in enumerate(text.splitlines(), start=1):
        rank = RANK.fullmatch(line)
        if rank is not None:
            process = int(rank.group(1))
            thread = int(rank.group(2))
            if process != 0:
                raise ValueError(
                    f"partition line {line_number} uses unexpected MPI rank "
                    f"{process}"
                )
            if thread >= thread_count:
                raise ValueError(
                    f"partition line {line_number} uses unexpected thread "
                    f"{thread}"
                )
            if thread in seen_sections:
                raise ValueError(f"partition repeats thread {thread}")
            seen_sections.add(thread)
            current_thread = thread
            continue
        component = COMPONENT.fullmatch(line)
        if component is not None:
            if current_thread is None:
                raise ValueError(
                    f"partition line {line_number} precedes a rank section"
                )
            components[current_thread].append(component.group(1))

    expected_tiles = set(active_tiles)
    tile_threads: dict[int, int] = {}
    for thread, names in enumerate(components):
        for name in names:
            match = TILE.fullmatch(name)
            if match is None:
                continue
            tile = int(match.group(1))
            if tile not in expected_tiles:
                raise ValueError(f"partition contains unexpected tile {tile}")
            if tile in tile_threads:
                raise ValueError(f"partition repeats active tile {tile}")
            tile_threads[tile] = thread

    missing = sorted(expected_tiles - set(tile_threads))
    if missing:
        preview = ",".join(str(tile) for tile in missing[:8])
        raise ValueError(f"partition omits active tile(s): {preview}")

    thread_records = []
    for thread, names in enumerate(components):
        active_count = sum(
            1 for tile_thread in tile_threads.values()
            if tile_thread == thread
        )
        if require_active_tile_per_thread and active_count == 0:
            raise ValueError(f"SST thread {thread} owns no active tile")
        thread_records.append(
            {
                "thread": thread,
                "component_count": len(names),
                "active_tile_count": active_count,
            }
        )

    return {
        "schema": "golem.sculptor-sst-partition",
        "schema_version": 1,
        "status": "PASS",
        "requested_threads": thread_count,
        "occupied_component_threads": sum(
            record["component_count"] > 0 for record in thread_records
        ),
        "occupied_active_tile_threads": sum(
            record["active_tile_count"] > 0 for record in thread_records
        ),
        "active_tile_count": len(active_tiles),
        "threads": thread_records,
        "tile_threads": {
            str(tile): tile_threads[tile] for tile in sorted(tile_threads)
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition", type=Path, required=True)
    parser.add_argument("--active-cores", type=Path, required=True)
    parser.add_argument("--threads", type=int, required=True)
    parser.add_argument("--require-active-tile-per-thread", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    result = validate_partition(
        args.partition.read_text(encoding="utf-8"),
        read_active_tiles(args.active_cores),
        args.threads,
        args.require_active_tile_per_thread,
    )
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "SST partition: PASS "
        f"({result['occupied_active_tile_threads']}/"
        f"{result['requested_threads']} active-tile threads)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
