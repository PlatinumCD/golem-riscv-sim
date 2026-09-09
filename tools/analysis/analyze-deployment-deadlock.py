#!/usr/bin/env python3

"""Build a route-level deadlock certificate from a Sculptor SST attempt."""

import argparse
import csv
import json
import mmap
import re
from collections import defaultdict
from pathlib import Path


PROGRESS_PREFIX = "SCULPTOR_RA_PROGRESS "
INVALID_U32 = (1 << 32) - 1
INVALID_U64 = (1 << 64) - 1
ATTRIBUTE_PATTERN = re.compile(
    r"([A-Za-z][A-Za-z0-9_]*)\s*=\s*(-?[0-9]+)\s*:\s*i[0-9]+"
)


def parse_progress_line(line):
    if not line.startswith(PROGRESS_PREFIX):
        return None
    fields = {}
    for token in line[len(PROGRESS_PREFIX) :].split():
        if "=" not in token:
            continue
        name, value = token.split("=", 1)
        if value.isdigit():
            fields[name] = int(value)
    required = {"tile", "steps", "wait_route", "wait_iteration"}
    if not required.issubset(fields):
        return None
    return fields


def load_final_progress(uart_directory):
    snapshots = {}
    for path in sorted(uart_directory.glob("tile-*.log")):
        final = None
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                parsed = parse_progress_line(line.rstrip("\n"))
                if parsed is not None:
                    final = parsed
        if final is not None:
            tile = final["tile"]
            if tile in snapshots:
                raise RuntimeError(f"duplicate final snapshot for tile {tile}")
            snapshots[tile] = final
    if not snapshots:
        raise RuntimeError(f"no deployment progress snapshots in {uart_directory}")
    return snapshots


def load_route(core_directory, destination_core, route_id):
    path = core_directory / f"core-{destination_core}-runtime-graph.mlir"
    if not path.is_file():
        raise RuntimeError(f"missing runtime graph for tile {destination_core}: {path}")
    marker = (
        f"#sculptor.deployment_route<id = {route_id} : i64"
    ).encode("ascii")
    with path.open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            start = data.find(marker)
            if start < 0:
                raise RuntimeError(
                    f"tile {destination_core} wait route {route_id} is absent from {path}"
                )
            end = data.find(b">", start)
            if end < 0:
                raise RuntimeError(f"unterminated route {route_id} in {path}")
            record = data[start : end + 1].decode("ascii")
    values = {
        name: int(value) for name, value in ATTRIBUTE_PATTERN.findall(record)
    }
    required = {
        "id",
        "sourceCore",
        "sourceTask",
        "destinationCore",
        "destinationTask",
        "sourceLoop",
        "destinationLoop",
        "producerToConsumerDelta",
    }
    if not required.issubset(values):
        missing = ", ".join(sorted(required - values.keys()))
        raise RuntimeError(f"route {route_id} is missing attributes: {missing}")
    if values["id"] != route_id or values["destinationCore"] != destination_core:
        raise RuntimeError(
            f"route {route_id} does not terminate at tile {destination_core}"
        )
    return values


def producer_iteration(route, consumer_iteration):
    delta = route["producerToConsumerDelta"]
    result = consumer_iteration - delta
    return result if result >= 0 else None


def build_wait_edges(snapshots, core_directory):
    edges = []
    for tile, snapshot in sorted(snapshots.items()):
        route_id = snapshot["wait_route"]
        iteration = snapshot["wait_iteration"]
        if route_id == INVALID_U32 or iteration == INVALID_U64:
            continue
        route = load_route(core_directory, tile, route_id)
        edges.append(
            {
                "destination_core": tile,
                "destination_loop": snapshot.get("wait_loop", INVALID_U32),
                "consumer_iteration": iteration,
                "route_id": route_id,
                "source_core": route["sourceCore"],
                "source_loop": route["sourceLoop"],
                "source_task": route["sourceTask"],
                "destination_task": route["destinationTask"],
                "producer_iteration": producer_iteration(route, iteration),
                "snapshot_steps": snapshot["steps"],
                "snapshot_retired": snapshot.get("retired", 0),
                "snapshot_pending_receive": snapshot.get("pending_receive", 0),
            }
        )
    return edges


def strongly_connected_components(edges):
    adjacency = defaultdict(list)
    nodes = set()
    for edge in edges:
        destination = edge["destination_core"]
        source = edge["source_core"]
        adjacency[destination].append(source)
        nodes.add(destination)
        nodes.add(source)

    index = 0
    indices = {}
    lowlinks = {}
    stack = []
    on_stack = set()
    components = []

    def visit(node):
        nonlocal index
        indices[node] = index
        lowlinks[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for successor in adjacency[node]:
            if successor not in indices:
                visit(successor)
                lowlinks[node] = min(lowlinks[node], lowlinks[successor])
            elif successor in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[successor])
        if lowlinks[node] != indices[node]:
            return
        component = []
        while True:
            member = stack.pop()
            on_stack.remove(member)
            component.append(member)
            if member == node:
                break
        components.append(sorted(component))

    for node in sorted(nodes):
        if node not in indices:
            visit(node)

    self_edges = {
        edge["destination_core"]
        for edge in edges
        if edge["destination_core"] == edge["source_core"]
    }
    return sorted(
        (
            component
            for component in components
            if len(component) > 1 or component[0] in self_edges
        ),
        key=lambda component: (-len(component), component),
    )


def collect_network_evidence(trace_directory, edges):
    targets = {
        (edge["route_id"], edge["consumer_iteration"]) for edge in edges
    }
    evidence = {
        target: {
            "injected_packets": 0,
            "arrived_packets": 0,
            "dma_scheduled": 0,
            "dma_completed": 0,
        }
        for target in targets
    }
    if trace_directory is None:
        return evidence, False

    for path in sorted(trace_directory.glob("tile-*-network.csv")):
        with path.open("r", encoding="utf-8", newline="") as stream:
            for row in csv.DictReader(stream):
                try:
                    key = (int(row["route_id"]), int(row["logical_iteration"]))
                except (KeyError, ValueError):
                    continue
                if key not in evidence:
                    continue
                if row.get("event") == "inject":
                    evidence[key]["injected_packets"] += 1
                elif row.get("event") == "arrive":
                    evidence[key]["arrived_packets"] += 1

    for path in sorted(trace_directory.glob("tile-*-receive-dma.csv")):
        with path.open("r", encoding="utf-8", newline="") as stream:
            for row in csv.DictReader(stream):
                try:
                    key = (int(row["route_id"]), int(row["logical_iteration"]))
                except (KeyError, ValueError):
                    continue
                if key not in evidence:
                    continue
                if row.get("event") == "schedule":
                    evidence[key]["dma_scheduled"] += 1
                elif row.get("event") == "complete":
                    evidence[key]["dma_completed"] += 1
    return evidence, True


def classify_evidence(counts, trace_available):
    if not trace_available:
        return "trace-unavailable"
    if counts["dma_completed"]:
        return "dma-complete-but-consumer-blocked"
    if counts["dma_scheduled"]:
        return "dma-scheduled-not-complete"
    if counts["arrived_packets"]:
        return "arrived-not-dma-scheduled"
    if counts["injected_packets"]:
        return "injected-not-arrived"
    return "not-injected"


def build_certificate(uart_directory, core_directory, trace_directory):
    snapshots = load_final_progress(uart_directory)
    edges = build_wait_edges(snapshots, core_directory)
    evidence, trace_available = collect_network_evidence(trace_directory, edges)
    for edge in edges:
        key = (edge["route_id"], edge["consumer_iteration"])
        edge["evidence"] = evidence[key]
        edge["classification"] = classify_evidence(
            evidence[key], trace_available
        )
    cycles = strongly_connected_components(edges)
    cycle_nodes = {node for component in cycles for node in component}
    cycle_edges = [
        edge
        for edge in edges
        if edge["destination_core"] in cycle_nodes
        and edge["source_core"] in cycle_nodes
    ]
    return {
        "schema": "golem.deployment-deadlock-certificate",
        "schema_version": 1,
        "snapshot_tiles": len(snapshots),
        "waiting_tiles": len(edges),
        "trace_available": trace_available,
        "cycles": cycles,
        "cycle_edges": cycle_edges,
        "wait_edges": edges,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uart-directory", required=True, type=Path)
    parser.add_argument("--core-directory", required=True, type=Path)
    parser.add_argument("--trace-directory", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    if not args.uart_directory.is_dir():
        raise RuntimeError(f"missing UART directory: {args.uart_directory}")
    if not args.core_directory.is_dir():
        raise RuntimeError(f"missing core directory: {args.core_directory}")
    if args.trace_directory is not None and not args.trace_directory.is_dir():
        raise RuntimeError(f"missing trace directory: {args.trace_directory}")

    certificate = build_certificate(
        args.uart_directory, args.core_directory, args.trace_directory
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(certificate, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    cycle_sizes = ",".join(str(len(cycle)) for cycle in certificate["cycles"])
    print(
        "deployment deadlock certificate: "
        f"snapshots={certificate['snapshot_tiles']} "
        f"waiters={certificate['waiting_tiles']} "
        f"cycles={len(certificate['cycles'])} "
        f"cycle_sizes={cycle_sizes or 'none'}"
    )


if __name__ == "__main__":
    main()
