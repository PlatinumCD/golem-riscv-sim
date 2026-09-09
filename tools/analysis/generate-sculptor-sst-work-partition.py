#!/usr/bin/env python3
"""Build a workload-balanced, locality-refined SST thread partition.

The stock SST simple partitioner balances top-level component count.  That is
not a useful proxy for a Sculptor deployment because nearly all host work is
executing the QEMU guest attached to each active tile.  This tool derives a
static tile-work estimate from the compiler's materialization audit, balances
that work with longest-processing-time placement, and then removes mesh cuts
without exceeding the requested load cap.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import random
import re


SCHEMA = "golem.sculptor-sst-work-partition"
SCHEMA_VERSION = 1
WORK_FIELDS = (
    "retained_output_physical_request_count_before_elision",
    "retained_input_physical_request_count_before_elision",
    "materialized_input_physical_request_count",
    "materialized_output_physical_request_count",
)
EXECUTION_EPOCH_PATTERN = re.compile(
    r"sculptor\.materialization\.execution_epoch_id = (\d+) : i64"
)
PHYSICAL_EXECUTION_COUNT_PATTERN = re.compile(
    r"sculptor\.parametric\.physical_execution_count = (\d+) : i64"
)
PROFILE_DURATION_PATTERN = re.compile(r"([0-9]+(?:\.[0-9]+)?) (s|ms|us|ns)")
PROFILE_DURATION_SCALE_NS = {
    "s": 1_000_000_000,
    "ms": 1_000_000,
    "us": 1_000,
    "ns": 1,
}


def unsigned(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{name} must be an unsigned integer")
    return value


def mesh_adjacency(rows: int, columns: int) -> list[list[int]]:
    result: list[list[int]] = []
    for tile in range(rows * columns):
        x = tile % columns
        y = tile // columns
        neighbors = []
        if x > 0:
            neighbors.append(tile - 1)
        if x + 1 < columns:
            neighbors.append(tile + 1)
        if y > 0:
            neighbors.append(tile - columns)
        if y + 1 < rows:
            neighbors.append(tile + columns)
        result.append(neighbors)
    return result


def edge_cut_count(assignments: list[int], adjacency: list[list[int]]) -> int:
    return sum(
        assignments[tile] != assignments[neighbor]
        for tile, neighbors in enumerate(adjacency)
        for neighbor in neighbors
    ) // 2


def weighted_edge_cut(
    assignments: list[int], edge_weights: list[dict[int, int]]
) -> int:
    return sum(
        weight
        for tile, neighbors in enumerate(edge_weights)
        for neighbor, weight in neighbors.items()
        if tile < neighbor and assignments[tile] != assignments[neighbor]
    )


def read_router_traffic_weights(
    path: Path, rows: int, columns: int
) -> tuple[list[dict[int, int]], bytes, int]:
    """Read bidirectional per-mesh-edge flit counts from an SST statistics CSV."""
    data = path.read_bytes()
    network_size = rows * columns
    result: list[dict[int, int]] = [dict() for _ in range(network_size)]
    directions = {
        "east": (1, 0),
        "west": (-1, 0),
        "north": (0, -1),
        "south": (0, 1),
    }
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {
            "ComponentName",
            "StatisticName",
            "StatisticSubId",
            "Sum.u64",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError("router statistics CSV lacks required columns")
        for row_index, record in enumerate(reader, start=2):
            direction = record["StatisticSubId"]
            if record["StatisticName"] != "flits_forwarded" or direction not in directions:
                continue
            match = re.fullmatch(r"router_(\d+)_(\d+)", record["ComponentName"])
            if match is None:
                continue
            x, y = (int(value) for value in match.groups())
            dx, dy = directions[direction]
            target_x, target_y = x + dx, y + dy
            if not (0 <= x < columns and 0 <= y < rows):
                raise ValueError(f"router statistics row {row_index} lies outside the mesh")
            if not (0 <= target_x < columns and 0 <= target_y < rows):
                continue
            try:
                flits = int(record["Sum.u64"])
            except ValueError as error:
                raise ValueError(
                    f"router statistics row {row_index} has an invalid flit count"
                ) from error
            if flits < 0:
                raise ValueError(
                    f"router statistics row {row_index} has a negative flit count"
                )
            source = y * columns + x
            target = target_y * columns + target_x
            result[source][target] = result[source].get(target, 0) + flits
            result[target][source] = result[target].get(source, 0) + flits
    total_flits = sum(sum(neighbors.values()) for neighbors in result) // 2
    if total_flits == 0:
        raise ValueError("router statistics contain no mesh traffic")
    return result, data, total_flits


def initial_lpt_partition(weights: list[int], threads: int) -> tuple[list[int], list[int]]:
    assignments = [-1] * len(weights)
    loads = [0] * threads
    for tile in sorted(
        range(len(weights)), key=lambda candidate: (weights[candidate], candidate), reverse=True
    ):
        thread = min(range(threads), key=lambda candidate: (loads[candidate], candidate))
        assignments[tile] = thread
        loads[thread] += weights[tile]
    return assignments, loads


def read_phase_work_vectors(
    records: list[dict[str, object]],
    weights: list[int],
    network_size: int,
) -> tuple[list[dict[int, float]], dict[str, object]]:
    """Derive a per-tile execution-frontier vector from finalized tile MLIR.

    The scalar materialization-audit weight remains the calibrated total cost
    for a tile.  Finalized task metadata divides that total across startup and
    execution epochs, exposing synchronization imbalance that a lifetime total
    necessarily hides.  Phase zero is startup; execution epoch N is phase N+1.
    """
    raw_vectors: list[dict[int, int]] = [dict() for _ in range(network_size)]
    source_digest = hashlib.sha256()
    task_count = 0
    execution_epochs: set[int] = set()

    for index, record in enumerate(records):
        tile = unsigned(record.get("tile_id"), f"tiles[{index}].tile_id")
        source_value = record.get("source")
        expected_sha256 = record.get("sha256")
        if not isinstance(source_value, str) or not source_value:
            raise ValueError(f"tiles[{index}].source is not a path")
        if not isinstance(expected_sha256, str) or re.fullmatch(
            r"[0-9a-f]{64}", expected_sha256
        ) is None:
            raise ValueError(f"tiles[{index}].sha256 is invalid")
        source = Path(source_value)
        try:
            source_bytes = source.read_bytes()
        except OSError as error:
            raise ValueError(
                f"cannot read finalized tile source for tile {tile}: {source}"
            ) from error
        actual_sha256 = hashlib.sha256(source_bytes).hexdigest()
        if actual_sha256 != expected_sha256:
            raise ValueError(
                f"finalized tile source hash mismatch for tile {tile}: {source}"
            )
        source_digest.update(f"{tile}:{actual_sha256}\n".encode("ascii"))

        for line in source_bytes.decode("utf-8").splitlines():
            if "sculptor.task.create" not in line:
                continue
            epoch_matches = EXECUTION_EPOCH_PATTERN.findall(line)
            if len(epoch_matches) > 1:
                raise ValueError(
                    f"tile {tile} task has multiple execution epoch identifiers"
                )
            if epoch_matches:
                epoch = int(epoch_matches[0])
                execution_epochs.add(epoch)
                phase = epoch + 1
            else:
                phase = 0
            count_matches = PHYSICAL_EXECUTION_COUNT_PATTERN.findall(line)
            if len(count_matches) > 1:
                raise ValueError(
                    f"tile {tile} task has multiple physical execution counts"
                )
            task_weight = int(count_matches[0]) if count_matches else 1
            if task_weight <= 0:
                raise ValueError(f"tile {tile} task has a non-positive work count")
            raw_vectors[tile][phase] = (
                raw_vectors[tile].get(phase, 0) + task_weight
            )
            task_count += 1

    if task_count == 0:
        raise ValueError("finalized tile sources contain no Sculptor tasks")

    phase_vectors: list[dict[int, float]] = [dict() for _ in range(network_size)]
    for tile, raw_vector in enumerate(raw_vectors):
        if weights[tile] == 0:
            continue
        raw_total = sum(raw_vector.values())
        if raw_total == 0:
            # An active controller-only tile still consumes host work during
            # startup even if it has no materialized task in finalized MLIR.
            phase_vectors[tile][0] = float(weights[tile])
            continue
        phase_vectors[tile] = {
            phase: weights[tile] * raw_weight / raw_total
            for phase, raw_weight in raw_vector.items()
        }

    phase_count = max(
        (phase for vector in phase_vectors for phase in vector), default=0
    ) + 1
    return phase_vectors, {
        "source_manifest_sha256": source_digest.hexdigest(),
        "source_count": len(records),
        "task_count": task_count,
        "execution_epoch_count": len(execution_epochs),
        "phase_count": phase_count,
    }


def profile_duration_nanoseconds(value: object, name: str) -> int:
    if not isinstance(value, str):
        raise ValueError(f"{name} is not a profile duration")
    match = PROFILE_DURATION_PATTERN.fullmatch(value)
    if match is None:
        raise ValueError(f"{name} is not a supported profile duration")
    return round(float(match.group(1)) * PROFILE_DURATION_SCALE_NS[match.group(2)])


def read_profile_work_weights(
    path: Path, rows: int, columns: int
) -> tuple[list[int], bytes, dict[str, object]]:
    """Read measured tile and router handler cost from an SST core profile."""
    data = path.read_bytes()
    profile = json.loads(data)
    components = profile.get("events")
    if not isinstance(components, dict):
        raise ValueError("SST core profile has no component event records")
    network_size = rows * columns
    weights = [1] * network_size
    tile_records = 0
    router_records = 0
    for name, record in components.items():
        if not isinstance(record, dict) or "recv_time" not in record:
            continue
        duration = profile_duration_nanoseconds(
            record["recv_time"], f"events.{name}.recv_time"
        )
        tile_match = re.fullmatch(r"tile(\d+)", name)
        router_match = re.fullmatch(r"router_(\d+)_(\d+)", name)
        if tile_match is not None:
            tile = int(tile_match.group(1))
            if tile >= network_size:
                raise ValueError("SST core profile tile lies outside the mesh")
            weights[tile] += duration
            tile_records += 1
        elif router_match is not None:
            x, y = (int(value) for value in router_match.groups())
            if not (0 <= x < columns and 0 <= y < rows):
                raise ValueError("SST core profile router lies outside the mesh")
            weights[y * columns + x] += duration
            router_records += 1
    if tile_records == 0 or router_records != network_size:
        raise ValueError("SST core profile has incomplete tile/router timing")
    return weights, data, {
        "source": "sst-core component event handler profile",
        "profile_sha256": hashlib.sha256(data).hexdigest(),
        "tile_record_count": tile_records,
        "router_record_count": router_records,
        "unit": "host nanoseconds",
    }


def phase_loads_for_partition(
    assignments: list[int],
    phase_vectors: list[dict[int, float]],
    threads: int,
    phase_count: int,
) -> list[list[float]]:
    result = [[0.0] * threads for _ in range(phase_count)]
    for tile, thread in enumerate(assignments):
        for phase, weight in phase_vectors[tile].items():
            result[phase][thread] += weight
    return result


def phase_critical_load(phase_loads: list[list[float]]) -> float:
    return sum(max(loads) for loads in phase_loads)


def refine_partition_phase_swaps(
    assignments: list[int],
    loads: list[int],
    weights: list[int],
    active_tiles: set[int],
    phase_vectors: list[dict[int, float]],
    edge_weights: list[dict[int, int]],
    maximum_load: float,
    maximum_weighted_cut: float,
    maximum_swaps: int,
) -> tuple[list[int], list[int], list[list[float]], int, int]:
    """Balance work at every synchronization frontier using bounded swaps."""
    assignments = list(assignments)
    loads = list(loads)
    thread_count = len(loads)
    phase_count = max(
        (phase for vector in phase_vectors for phase in vector), default=0
    ) + 1
    phase_loads = phase_loads_for_partition(
        assignments, phase_vectors, thread_count, phase_count
    )
    active_counts = [0] * thread_count
    for tile in active_tiles:
        active_counts[assignments[tile]] += 1
    completed_swaps = 0
    current_weighted_cut = weighted_edge_cut(assignments, edge_weights)

    for _ in range(maximum_swaps):
        phase_maxima = [max(values) for values in phase_loads]
        unaffected_max = [
            [
                [
                    max(
                        (
                            value
                            for thread, value in enumerate(values)
                            if thread != first and thread != second
                        ),
                        default=0.0,
                    )
                    for second in range(thread_count)
                ]
                for first in range(thread_count)
            ]
            for values in phase_loads
        ]
        best: tuple[tuple[float, int, float, int, int], int, int] | None = None
        for first in range(len(assignments)):
            source = assignments[first]
            first_active = first in active_tiles
            for second in range(first + 1, len(assignments)):
                destination = assignments[second]
                if source == destination:
                    continue
                source_load = loads[source] - weights[first] + weights[second]
                destination_load = (
                    loads[destination] - weights[second] + weights[first]
                )
                if source_load > maximum_load or destination_load > maximum_load:
                    continue
                second_active = second in active_tiles
                if first_active and not second_active and active_counts[source] == 1:
                    continue
                if second_active and not first_active and active_counts[destination] == 1:
                    continue

                phase_delta = 0.0
                affected_phases = (
                    phase_vectors[first].keys() | phase_vectors[second].keys()
                )
                for phase in affected_phases:
                    first_weight = phase_vectors[first].get(phase, 0.0)
                    second_weight = phase_vectors[second].get(phase, 0.0)
                    new_source = (
                        phase_loads[phase][source] - first_weight + second_weight
                    )
                    new_destination = (
                        phase_loads[phase][destination]
                        - second_weight
                        + first_weight
                    )
                    new_maximum = max(
                        unaffected_max[phase][source][destination],
                        new_source,
                        new_destination,
                    )
                    phase_delta += new_maximum - phase_maxima[phase]
                if phase_delta >= -1.0e-9:
                    continue
                cut_delta = swap_cut_delta(
                    assignments, edge_weights, first, second
                )
                if current_weighted_cut + cut_delta > maximum_weighted_cut:
                    continue
                score = (
                    phase_delta,
                    cut_delta,
                    max(source_load, destination_load),
                    first,
                    second,
                )
                if best is None or score < best[0]:
                    best = (score, first, second)
        if best is None:
            break

        _, first, second = best
        source = assignments[first]
        destination = assignments[second]
        first_weight = weights[first]
        second_weight = weights[second]
        loads[source] += second_weight - first_weight
        loads[destination] += first_weight - second_weight
        for phase in phase_vectors[first].keys() | phase_vectors[second].keys():
            first_phase_weight = phase_vectors[first].get(phase, 0.0)
            second_phase_weight = phase_vectors[second].get(phase, 0.0)
            phase_loads[phase][source] += second_phase_weight - first_phase_weight
            phase_loads[phase][destination] += first_phase_weight - second_phase_weight
        first_active = first in active_tiles
        second_active = second in active_tiles
        active_counts[source] += int(second_active) - int(first_active)
        active_counts[destination] += int(first_active) - int(second_active)
        current_weighted_cut += swap_cut_delta(
            assignments, edge_weights, first, second
        )
        assignments[first], assignments[second] = destination, source
        completed_swaps += 1

    return (
        assignments,
        loads,
        phase_loads,
        completed_swaps,
        current_weighted_cut,
    )


def refine_partition(
    initial_assignments: list[int],
    initial_loads: list[int],
    weights: list[int],
    active_tiles: set[int],
    adjacency: list[list[int]],
    edge_weights: list[dict[int, int]],
    maximum_load: float,
    seed: int,
) -> tuple[list[int], list[int]]:
    assignments = list(initial_assignments)
    loads = list(initial_loads)
    active_counts = [0] * len(loads)
    for tile in active_tiles:
        active_counts[assignments[tile]] += 1

    generator = random.Random(seed)
    for _ in range(100):
        changed = False
        order = list(range(len(assignments)))
        generator.shuffle(order)
        for tile in order:
            source = assignments[tile]
            source_neighbors = sum(
                weight
                for neighbor, weight in edge_weights[tile].items()
                if assignments[neighbor] == source
            )
            best: tuple[tuple[int, int, int], int] | None = None
            for destination in sorted(
                {assignments[neighbor] for neighbor in adjacency[tile]} - {source}
            ):
                cut_delta = source_neighbors - sum(
                    weight
                    for neighbor, weight in edge_weights[tile].items()
                    if assignments[neighbor] == destination
                )
                if cut_delta >= 0:
                    continue
                if loads[destination] + weights[tile] > maximum_load:
                    continue
                if tile in active_tiles and active_counts[source] == 1:
                    continue
                score = (cut_delta, loads[destination] + weights[tile], destination)
                if best is None or score < best[0]:
                    best = (score, destination)
            if best is None:
                continue
            destination = best[1]
            assignments[tile] = destination
            loads[source] -= weights[tile]
            loads[destination] += weights[tile]
            if tile in active_tiles:
                active_counts[source] -= 1
                active_counts[destination] += 1
            changed = True
        if not changed:
            break
    return assignments, loads


def swap_cut_delta(
    assignments: list[int],
    edge_weights: list[dict[int, int]],
    first: int,
    second: int,
) -> int:
    first_thread = assignments[first]
    second_thread = assignments[second]
    affected: set[tuple[int, int]] = set()
    for tile in (first, second):
        affected.update(
            (min(tile, neighbor), max(tile, neighbor))
            for neighbor in edge_weights[tile]
        )
    before = 0
    after = 0
    for source, target in affected:
        weight = edge_weights[source][target]
        source_before = assignments[source]
        target_before = assignments[target]
        before += weight * (source_before != target_before)
        source_after = (
            second_thread
            if source == first
            else first_thread if source == second else source_before
        )
        target_after = (
            second_thread
            if target == first
            else first_thread if target == second else target_before
        )
        after += weight * (source_after != target_after)
    return after - before


def refine_partition_swaps(
    assignments: list[int],
    loads: list[int],
    weights: list[int],
    active_tiles: set[int],
    edge_weights: list[dict[int, int]],
    maximum_load: float,
    seed: int,
) -> tuple[list[int], list[int]]:
    """Use load-preserving pair swaps to escape the single-move local minimum."""
    assignments = list(assignments)
    loads = list(loads)
    active_counts = [0] * len(loads)
    for tile in active_tiles:
        active_counts[assignments[tile]] += 1
    generator = random.Random(seed ^ 0x5A17)

    for _ in range(200):
        members = [list() for _ in loads]
        for tile, thread in enumerate(assignments):
            members[thread].append(tile)
        order = list(range(len(assignments)))
        generator.shuffle(order)
        best: tuple[tuple[int, int, int, int, int], int, int] | None = None
        for first in order:
            source = assignments[first]
            destinations = {
                assignments[neighbor]
                for neighbor, edge_weight in edge_weights[first].items()
                if edge_weight > 1 and assignments[neighbor] != source
            }
            if not destinations:
                continue
            for destination in sorted(destinations):
                # Similar-weight partners preserve the LPT balance.  Trying the
                # closest 24 makes the search bounded while retaining ample
                # choices on the 20x20 model meshes.
                partners = sorted(
                    members[destination],
                    key=lambda tile: (abs(weights[tile] - weights[first]), tile),
                )[:24]
                for second in partners:
                    source_load = loads[source] - weights[first] + weights[second]
                    destination_load = (
                        loads[destination] - weights[second] + weights[first]
                    )
                    if source_load > maximum_load or destination_load > maximum_load:
                        continue
                    first_active = first in active_tiles
                    second_active = second in active_tiles
                    if first_active and not second_active and active_counts[source] == 1:
                        continue
                    if second_active and not first_active and active_counts[destination] == 1:
                        continue
                    cut_delta = swap_cut_delta(
                        assignments, edge_weights, first, second
                    )
                    if cut_delta >= 0:
                        continue
                    score = (
                        cut_delta,
                        max(source_load, destination_load),
                        abs(source_load - destination_load),
                        first,
                        second,
                    )
                    if best is None or score < best[0]:
                        best = (score, first, second)
        if best is None:
            break
        _, first, second = best
        source = assignments[first]
        destination = assignments[second]
        loads[source] += weights[second] - weights[first]
        loads[destination] += weights[first] - weights[second]
        first_active = first in active_tiles
        second_active = second in active_tiles
        active_counts[source] += int(second_active) - int(first_active)
        active_counts[destination] += int(first_active) - int(second_active)
        assignments[first], assignments[second] = destination, source
    return assignments, loads


def generate_partition(
    audit_path: Path,
    rows: int,
    columns: int,
    threads: int,
    base_tile_weight: int,
    maximum_load_factor: float,
    refinement_trials: int,
    router_statistics_path: Path | None,
    phase_aware: bool,
    maximum_phase_swaps: int,
    phase_cut_growth_factor: float,
    sst_core_profile_path: Path | None,
) -> dict[str, object]:
    audit_bytes = audit_path.read_bytes()
    audit = json.loads(audit_bytes)
    if (
        audit.get("schema") != "sculptor.materialization-audit"
        or audit.get("version") != 1
        or audit.get("status") != "PASS"
        or audit.get("errors") != []
    ):
        raise ValueError("materialization audit is not a passing V1 audit")

    network_size = rows * columns
    active_tile_ids = audit.get("active_tile_ids")
    if (
        not isinstance(active_tile_ids, list)
        or not active_tile_ids
        or active_tile_ids != sorted(set(active_tile_ids))
    ):
        raise ValueError("materialization audit has an invalid active tile list")
    active_tiles = {
        unsigned(tile, f"active_tile_ids[{index}]")
        for index, tile in enumerate(active_tile_ids)
    }
    if max(active_tiles) >= network_size:
        raise ValueError("active tile lies outside the requested mesh")
    if threads > len(active_tiles):
        raise ValueError("SST thread count exceeds the active tile count")

    records = audit.get("tiles")
    if not isinstance(records, list):
        raise ValueError("materialization audit has no per-tile records")
    weights = [0] * network_size
    seen: set[int] = set()
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            raise ValueError(f"tiles[{index}] is not an object")
        tile = unsigned(record.get("tile_id"), f"tiles[{index}].tile_id")
        if tile not in active_tiles or tile in seen:
            raise ValueError(f"materialization audit has invalid tile record {tile}")
        seen.add(tile)
        weights[tile] = base_tile_weight + sum(
            unsigned(record.get(field), f"tiles[{index}].{field}")
            for field in WORK_FIELDS
        )
    if seen != active_tiles:
        raise ValueError("materialization audit per-tile records are incomplete")

    profile_bytes = None
    profile_work_model = None
    if sst_core_profile_path is not None:
        weights, profile_bytes, profile_work_model = read_profile_work_weights(
            sst_core_profile_path, rows, columns
        )

    phase_vectors = None
    phase_metadata = None
    if phase_aware:
        phase_vectors, phase_metadata = read_phase_work_vectors(
            records, weights, network_size
        )

    adjacency = mesh_adjacency(rows, columns)
    traffic_bytes = None
    total_flits = 0
    if router_statistics_path is None:
        edge_weights = [
            {neighbor: 1 for neighbor in neighbors} for neighbors in adjacency
        ]
    else:
        traffic_weights, traffic_bytes, total_flits = read_router_traffic_weights(
            router_statistics_path, rows, columns
        )
        # Retain a unit topology cost so unused links do not become free cuts.
        edge_weights = [
            {
                neighbor: 1 + traffic_weights[tile].get(neighbor, 0)
                for neighbor in neighbors
            }
            for tile, neighbors in enumerate(adjacency)
        ]
    initial_assignments, initial_loads = initial_lpt_partition(weights, threads)
    average_load = sum(initial_loads) / threads
    maximum_load = average_load * maximum_load_factor
    candidates: list[dict[str, object]] = []
    for seed in range(refinement_trials):
        assignments, loads = refine_partition(
            initial_assignments,
            initial_loads,
            weights,
            active_tiles,
            adjacency,
            edge_weights,
            maximum_load,
            seed,
        )
        assignments, loads = refine_partition_swaps(
            assignments,
            loads,
            weights,
            active_tiles,
            edge_weights,
            maximum_load,
            seed,
        )
        phase_loads = None
        phase_critical_before = None
        completed_phase_swaps = 0
        phase_weighted_cut_before = None
        phase_weighted_cut_after = None
        if phase_vectors is not None and phase_metadata is not None:
            phase_loads = phase_loads_for_partition(
                assignments,
                phase_vectors,
                threads,
                int(phase_metadata["phase_count"]),
            )
            phase_critical_before = phase_critical_load(phase_loads)
            phase_weighted_cut_before = weighted_edge_cut(
                assignments, edge_weights
            )
            (
                assignments,
                loads,
                phase_loads,
                completed_phase_swaps,
                phase_weighted_cut_after,
            ) = (
                refine_partition_phase_swaps(
                    assignments,
                    loads,
                    weights,
                    active_tiles,
                    phase_vectors,
                    edge_weights,
                    maximum_load,
                    phase_weighted_cut_before * phase_cut_growth_factor,
                    maximum_phase_swaps,
                )
            )
        candidates.append(
            {
                "weighted_cut": weighted_edge_cut(assignments, edge_weights),
                "edge_cuts": edge_cut_count(assignments, adjacency),
                "seed": seed,
                "assignments": assignments,
                "loads": loads,
                "phase_loads": phase_loads,
                "phase_critical_before": phase_critical_before,
                "phase_critical_after": (
                    phase_critical_load(phase_loads)
                    if phase_loads is not None
                    else None
                ),
                "completed_phase_swaps": completed_phase_swaps,
                "phase_weighted_cut_before": phase_weighted_cut_before,
                "phase_weighted_cut_after": phase_weighted_cut_after,
            }
        )
    if phase_aware:
        best = min(
            candidates,
            key=lambda candidate: (
                candidate["phase_critical_after"],
                candidate["weighted_cut"],
                candidate["edge_cuts"],
                max(candidate["loads"]),
                max(candidate["loads"]) - min(candidate["loads"]),
                candidate["seed"],
            ),
        )
    else:
        best = min(
            candidates,
            key=lambda candidate: (
                candidate["weighted_cut"],
                candidate["edge_cuts"],
                max(candidate["loads"]),
                max(candidate["loads"]) - min(candidate["loads"]),
                sum(abs(load - average_load) for load in candidate["loads"]),
                candidate["seed"],
            ),
        )
    weighted_cut = int(best["weighted_cut"])
    edge_cuts = int(best["edge_cuts"])
    seed = int(best["seed"])
    assignments = best["assignments"]
    loads = best["loads"]
    active_counts = [0] * threads
    for tile in active_tiles:
        active_counts[assignments[tile]] += 1
    if any(count == 0 for count in active_counts):
        raise ValueError("generated partition left an SST thread without an active tile")

    controller_thread = min(
        range(threads), key=lambda candidate: (loads[candidate], candidate)
    )
    total_mesh_edges = rows * (columns - 1) + columns * (rows - 1)
    result = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "materialization_audit_sha256": hashlib.sha256(audit_bytes).hexdigest(),
        "mesh": {"rows": rows, "columns": columns},
        "thread_count": threads,
        "controller_thread": controller_thread,
        "mesh_threads": assignments,
        "active_tile_ids": sorted(active_tiles),
        "work_model": {
            "base_tile_weight": base_tile_weight,
            "fields": list(WORK_FIELDS),
        },
        "refinement": {
            "maximum_load_factor": maximum_load_factor,
            "trials": refinement_trials,
            "selected_seed": seed,
        },
        "predicted_thread_loads": loads,
        "active_tiles_per_thread": active_counts,
        "mesh_edge_cuts": edge_cuts,
        "mesh_edge_count": total_mesh_edges,
        "weighted_mesh_edge_cut": weighted_cut,
    }
    if profile_work_model is not None:
        result["work_model"] = profile_work_model
    if phase_aware and phase_metadata is not None:
        phase_loads = best["phase_loads"]
        result["phase_work_model"] = {
            **phase_metadata,
            "startup_phase": 0,
            "execution_epoch_phase_offset": 1,
            "normalization": "per-tile materialization-audit scalar weight",
            "maximum_swaps": maximum_phase_swaps,
            "maximum_cut_growth_factor": phase_cut_growth_factor,
            "completed_swaps": best["completed_phase_swaps"],
            "weighted_cut_before_refinement": best[
                "phase_weighted_cut_before"
            ],
            "weighted_cut_after_refinement": best[
                "phase_weighted_cut_after"
            ],
            "critical_load_before_refinement": best["phase_critical_before"],
            "critical_load_after_refinement": best["phase_critical_after"],
            "ideal_lower_bound": sum(
                sum(values) / threads for values in phase_loads
            ),
            "predicted_phase_thread_loads": phase_loads,
        }
    if router_statistics_path is not None:
        result["traffic_profile"] = {
            "router_statistics_sha256": hashlib.sha256(traffic_bytes).hexdigest(),
            "total_flits": total_flits,
            "cross_thread_flits": sum(
                weight
                for tile, neighbors in enumerate(edge_weights)
                for neighbor, weight in neighbors.items()
                if tile < neighbor
                and assignments[tile] != assignments[neighbor]
            ) - edge_cuts,
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--materialization-audit", type=Path, required=True)
    parser.add_argument("--mesh-rows", type=int, required=True)
    parser.add_argument("--mesh-columns", type=int, required=True)
    parser.add_argument("--threads", type=int, required=True)
    parser.add_argument("--base-tile-weight", type=int, default=7)
    parser.add_argument("--maximum-load-factor", type=float, default=1.02)
    parser.add_argument("--refinement-trials", type=int, default=16)
    parser.add_argument("--router-statistics", type=Path)
    parser.add_argument("--phase-aware", action="store_true")
    parser.add_argument("--maximum-phase-swaps", type=int, default=200)
    parser.add_argument("--phase-cut-growth-factor", type=float, default=1.0)
    parser.add_argument("--sst-core-profile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mesh_rows <= 0 or args.mesh_columns <= 0 or args.threads <= 0:
        parser.error("mesh dimensions and thread count must be positive")
    if args.base_tile_weight <= 0:
        parser.error("base tile weight must be positive")
    if args.maximum_load_factor < 1.0:
        parser.error("maximum load factor must be at least one")
    if args.refinement_trials <= 0:
        parser.error("refinement trial count must be positive")
    if args.maximum_phase_swaps < 0:
        parser.error("maximum phase swap count must be non-negative")
    if args.phase_cut_growth_factor < 1.0:
        parser.error("phase cut growth factor must be at least one")

    result = generate_partition(
        args.materialization_audit,
        args.mesh_rows,
        args.mesh_columns,
        args.threads,
        args.base_tile_weight,
        args.maximum_load_factor,
        args.refinement_trials,
        args.router_statistics,
        args.phase_aware,
        args.maximum_phase_swaps,
        args.phase_cut_growth_factor,
        args.sst_core_profile,
    )
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    loads = result["predicted_thread_loads"]
    print(
        "SST workload partition: "
        f"{result['mesh_edge_cuts']}/{result['mesh_edge_count']} mesh cuts, "
        f"predicted load range {min(loads)}..{max(loads)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
