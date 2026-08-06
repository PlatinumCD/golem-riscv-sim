#!/usr/bin/env python3

"""Convert a joined Mittens performance profile into a compact flow trace."""

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path


SCHEMA_VERSION = 2
ANALOG_OPERATIONS = {
    1: "set",
    2: "load",
    3: "compute",
    4: "store",
    5: "move",
}
TASK_ID_PATTERN = re.compile(
    r"sculptor\.deployment\.global_task_id\s*=\s*(-?\d+)\s*:\s*i64"
)


def string_attribute(text, name, default=""):
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\"([^\"]*)\"", text)
    return match.group(1) if match else default


def integer_attribute(text, name, default=0):
    match = re.search(
        rf"\b{re.escape(name)}\s*=\s*(-?\d+)\s*:\s*i64",
        text,
    )
    return int(match.group(1)) if match else default


def integer(row, name, default=0):
    value = row.get(name, "")
    return int(value) if value not in {"", None} else default


def read_csv(path):
    if not path.is_file():
        return []
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def read_json(path):
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def task_ir_files(path):
    if path is None:
        return []
    if path.is_file():
        return [path]
    isolated = sorted(path.glob("core-*-isolated.mlir"))
    if isolated:
        return isolated
    return sorted(path.rglob("*.mlir"))


def read_task_metadata(path):
    metadata = {}
    for ir_file in task_ir_files(path):
        with ir_file.open("r", encoding="utf-8") as stream:
            for line in stream:
                if "sculptor.task.create" not in line:
                    continue
                match = TASK_ID_PATTERN.search(line)
                if not match:
                    continue
                task_id = int(match.group(1))
                candidate = {
                    "name": string_attribute(line, "task_name"),
                    "kind": string_attribute(line, "task_kind"),
                    "domain": string_attribute(line, "domain"),
                    "sourceLayer": string_attribute(line, "source_layer"),
                    "localIndex": integer_attribute(
                        line,
                        "sculptor.runtime.task_index",
                        -1,
                    ),
                    "digitalOps": integer_attribute(
                        line,
                        "sculptor.runtime.digital_ops",
                        0,
                    ),
                }
                previous = metadata.get(task_id)
                if previous is not None and previous != candidate:
                    raise ValueError(
                        f"conflicting metadata for global task {task_id}: "
                        f"{ir_file}"
                    )
                metadata[task_id] = candidate
    return metadata


def read_critical_tasks(profile):
    result = {}
    for row in read_csv(profile / "critical-path.csv"):
        key = (integer(row, "execution_id"), integer(row, "task_id"))
        result[key] = {
            "critical": True,
            "criticalIndex": integer(row, "critical_index", -1),
            "predecessorType": row.get("predecessor_type", ""),
            "predecessorTask": integer(row, "predecessor_task", -1),
            "predecessorRoute": integer(row, "route_id", -1),
            "ready": integer(row, "ready_tick"),
            "waitTicks": integer(row, "wait_ticks"),
        }
    return result


def tile_path(source, destination, width):
    source_x = source % width
    source_y = source // width
    destination_x = destination % width
    destination_y = destination // width
    path = [source]
    x = source_x
    y = source_y

    while x != destination_x:
        x += 1 if destination_x > x else -1
        path.append(y * width + x)
    while y != destination_y:
        y += 1 if destination_y > y else -1
        path.append(y * width + x)
    return path


def route_queue_ticks(network_rows):
    queue_ticks = defaultdict(int)
    ready_ticks = {}
    for row in network_rows:
        key = (integer(row, "execution_id"), integer(row, "route_id"))
        queue_ticks[key] = max(
            queue_ticks[key],
            integer(row, "endpoint_queue_ticks"),
        )
        ready = integer(row, "ready_tick")
        if ready != 0:
            ready_ticks[key] = min(ready_ticks.get(key, ready), ready)
    return queue_ticks, ready_ticks


def emit_tasks(profile, metadata, critical_tasks):
    result = []
    for row in read_csv(profile / "tasks.csv"):
        start = integer(row, "start_tick")
        finish = integer(row, "finish_tick")
        if finish < start:
            raise ValueError("task finish precedes task start")
        task_id = integer(row, "task_id")
        execution = integer(row, "execution_id")
        event = {
            "tile": integer(row, "tile_id"),
            "task": task_id,
            "execution": execution,
            "start": start,
            "end": finish,
            "instructions": integer(row, "retired_instructions"),
            "cycles": integer(row, "cpu_cycles"),
            "critical": False,
        }
        event.update(metadata.get(task_id, {}))
        event.update(critical_tasks.get((execution, task_id), {}))
        result.append(event)
    return result


def emit_packets(profile, width, height):
    result = []
    tile_count = width * height
    for row in read_csv(profile / "network-packets.csv"):
        source = integer(row, "source")
        destination = integer(row, "destination")
        if source >= tile_count or destination >= tile_count:
            raise ValueError(
                f"packet uses tile outside {width}x{height}: "
                f"{source}->{destination}"
            )
        start = integer(row, "injection_tick")
        end = integer(row, "arrival_tick")
        result.append(
            {
                "packet": integer(row, "packet_id"),
                "route": integer(row, "route_id"),
                "execution": integer(row, "execution_id"),
                "kind": row.get("kind", "packet"),
                "source": source,
                "destination": destination,
                "words": integer(row, "words"),
                "protocolWords": integer(row, "protocol_words"),
                "payloadWords": integer(row, "payload_words"),
                "hops": integer(row, "hops"),
                "wordHops": integer(row, "word_hops"),
                "ready": integer(row, "ready_tick"),
                "start": start,
                "end": max(start, end),
                "queueTicks": integer(row, "endpoint_queue_ticks"),
                "transitTicks": integer(row, "transit_ticks"),
                "path": tile_path(source, destination, width),
            }
        )
    return result


def emit_routes(profile, width, height):
    network_rows = read_csv(profile / "network-packets.csv")
    queue_ticks, ready_ticks = route_queue_ticks(network_rows)
    result = []
    tile_count = width * height

    for row in read_csv(profile / "routes.csv"):
        source = integer(row, "source_core")
        destination = integer(row, "destination_core")
        if source >= tile_count or destination >= tile_count:
            raise ValueError(
                f"route uses tile outside {width}x{height}: "
                f"{source}->{destination}"
            )
        execution = integer(row, "execution_id")
        route_id = integer(row, "route_id")
        key = (execution, route_id)
        start = integer(row, "injection_start_tick")
        end = integer(row, "arrival_finish_tick")
        path = tile_path(source, destination, width)
        payload_words = integer(row, "payload_words")
        protocol_words = integer(row, "protocol_words")
        queue = queue_ticks.get(key, 0)
        ready = ready_ticks.get(key, start)
        result.append(
            {
                "route": route_id,
                "execution": execution,
                "source": source,
                "destination": destination,
                "sourceTask": integer(row, "source_task", -1),
                "destinationTask": integer(row, "destination_task", -1),
                "resource": integer(row, "resource_id", -1),
                "start": start,
                "end": max(start, end),
                "dmaEnd": integer(row, "dma_finish_tick", end),
                "ready": ready,
                "words": payload_words + protocol_words,
                "payloadWords": payload_words,
                "hops": max(0, len(path) - 1),
                "path": path,
                "queueTicks": queue,
                "contended": queue > 0 or start > ready,
            }
        )
    return result


def emit_dma(profile):
    result = []
    for row in read_csv(profile / "receive-dma.csv"):
        start = integer(row, "schedule_tick")
        end = integer(row, "completion_tick")
        result.append(
            {
                "tile": integer(row, "tile_id"),
                "source": integer(row, "source"),
                "route": integer(row, "route_id"),
                "execution": integer(row, "execution_id"),
                "start": start,
                "end": max(start, end),
                "words": integer(row, "words"),
                "cycles": integer(row, "service_cycles"),
            }
        )
    return result


def emit_analog(profile):
    result = []
    for row in read_csv(profile / "analog-operations.csv"):
        start = integer(row, "start_tick")
        end = integer(row, "finish_tick")
        operation_id = integer(row, "operation")
        result.append(
            {
                "tile": integer(row, "tile_id"),
                "array": integer(row, "array_id"),
                "ticket": integer(row, "ticket"),
                "operation": ANALOG_OPERATIONS.get(
                    operation_id,
                    f"operation-{operation_id}",
                ),
                "submitted": integer(row, "submitted_tick"),
                "start": start,
                "end": max(start, end),
                "queueTicks": integer(row, "queue_ticks"),
                "inputTicks": integer(row, "input_transfer_ticks"),
                "computeTicks": integer(row, "compute_ticks"),
                "outputTicks": integer(row, "output_transfer_ticks"),
            }
        )
    return result


def emit_waits(profile):
    result = []
    for row in read_csv(profile / "waits.csv"):
        duration = integer(row, "duration_ticks")
        if duration <= 0:
            continue
        start = integer(row, "start_tick")
        result.append(
            {
                "tile": integer(row, "tile_id"),
                "reason": row.get("reason", "wait"),
                "start": start,
                "end": start + duration,
            }
        )
    return result


def emit_blocked(profile):
    result = []
    for row in read_csv(profile / "transmit-blocked.csv"):
        start = integer(row, "start_tick")
        finish = integer(row, "finish_tick")
        result.append(
            {
                "tile": integer(row, "tile_id"),
                "route": integer(row, "route_id"),
                "execution": integer(row, "execution_id"),
                "destination": integer(row, "destination"),
                "kind": row.get("kind", "packet"),
                "words": integer(row, "words"),
                "start": start,
                "end": max(start, finish),
                "retries": integer(row, "retry_count"),
                "maximumQueueOccupancy": integer(
                    row,
                    "maximum_queue_occupancy",
                ),
            }
        )
    return result


def link_summary(routes):
    links = {}
    for route in routes:
        for source, destination in zip(route["path"], route["path"][1:]):
            key = (min(source, destination), max(source, destination))
            if key not in links:
                links[key] = {
                    "source": key[0],
                    "destination": key[1],
                    "words": 0,
                    "routes": 0,
                    "contendedRoutes": 0,
                }
            link = links[key]
            link["words"] += route["words"]
            link["routes"] += 1
            link["contendedRoutes"] += int(route["contended"])
    return sorted(
        links.values(),
        key=lambda link: (link["source"], link["destination"]),
    )


def timeline_bounds(summary, collections):
    starts = []
    ends = []
    for collection in collections:
        for event in collection:
            if "start" in event:
                starts.append(event["start"])
            if "end" in event:
                ends.append(event["end"])
            if "dmaEnd" in event:
                ends.append(event["dmaEnd"])
    finish = integer(summary, "finish_tick")
    if finish == 0:
        finish = max(ends, default=1)
    return min(starts, default=0), max(finish, max(ends, default=finish))


def normalize_times(data, origin):
    time_fields = {
        "start",
        "end",
        "dmaEnd",
        "ready",
        "submitted",
    }
    for collection_name in (
        "tasks",
        "routes",
        "packets",
        "dma",
        "analog",
        "waits",
        "blocked",
    ):
        for event in data[collection_name]:
            for field in time_fields.intersection(event):
                event[field] -= origin


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Export task, route, DMA, analog, and wait activity for the "
            "Mittens web visualizer"
        )
    )
    parser.add_argument("profile_directory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=8)
    parser.add_argument("--height", type=int, default=8)
    parser.add_argument("--title", default="Mittens mesh activity")
    parser.add_argument("--source", default="")
    parser.add_argument(
        "--task-ir",
        type=Path,
        default=None,
        help=(
            "Optional task IR file or directory containing core-*-isolated.mlir "
            "files; global task metadata is joined into task events"
        ),
    )
    arguments = parser.parse_args()

    if arguments.width <= 0 or arguments.height <= 0:
        raise ValueError("mesh dimensions must be positive")

    profile = arguments.profile_directory
    summary = read_json(profile / "summary.json")
    if not summary:
        raise FileNotFoundError(f"missing profile summary: {profile}")

    metadata = read_task_metadata(arguments.task_ir)
    critical_tasks = read_critical_tasks(profile)
    tasks = emit_tasks(profile, metadata, critical_tasks)
    routes = emit_routes(profile, arguments.width, arguments.height)
    packets = emit_packets(profile, arguments.width, arguments.height)
    dma = emit_dma(profile)
    analog = emit_analog(profile)
    waits = emit_waits(profile)
    blocked = emit_blocked(profile)
    start, finish = timeline_bounds(
        summary,
        (tasks, routes, packets, dma, analog, waits, blocked),
    )

    data = {
        "schemaVersion": SCHEMA_VERSION,
        "meta": {
            "title": arguments.title,
            "source": arguments.source,
            "width": arguments.width,
            "height": arguments.height,
            "timebasePs": float(summary.get("timebase_ps", 1.0)),
            "originTick": start,
            "durationTicks": max(1, finish - start),
            "routing": "xy",
            "detail": "measured-activity-with-logical-xy-motion",
            "timingFidelity": {
                "measured": [
                    "task intervals",
                    "packet injection and arrival endpoints",
                    "receive DMA intervals",
                    "analog operation intervals",
                    "wait intervals",
                    "transmit backpressure intervals",
                ],
                "reconstructed": [
                    "intermediate packet position along deterministic XY path",
                ],
            },
        },
        "summary": {
            "tasks": len(tasks),
            "routes": len(routes),
            "packets": len(packets),
            "dma": len(dma),
            "analog": len(analog),
            "waits": len(waits),
            "blocked": len(blocked),
            "criticalTasks": len(critical_tasks),
            "contendedRoutes": sum(route["contended"] for route in routes),
            "injectedWords": sum(route["words"] for route in routes),
            "activeTiles": len(
                {
                    event["tile"]
                    for collection in (tasks, dma, analog, waits)
                    for event in collection
                }
                | {
                    tile
                    for route in routes
                    for tile in (route["source"], route["destination"])
                }
            ),
        },
        "tasks": tasks,
        "routes": routes,
        "packets": packets,
        "dma": dma,
        "analog": analog,
        "waits": waits,
        "blocked": blocked,
        "links": link_summary(routes),
    }
    normalize_times(data, start)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", encoding="utf-8") as stream:
        json.dump(data, stream, separators=(",", ":"))
        stream.write("\n")

    print(
        f"visualization trace: {arguments.output} "
        f"({len(tasks)} tasks, {len(routes)} routes, "
        f"{len(packets)} packets, {len(analog)} analog operations, "
        f"{len(waits)} waits, {len(blocked)} blocked intervals)"
    )


if __name__ == "__main__":
    main()
