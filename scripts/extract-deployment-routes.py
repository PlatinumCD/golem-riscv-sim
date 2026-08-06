#!/usr/bin/env python3

import argparse
import csv
import re
from pathlib import Path


ROUTE_PATTERN = re.compile(
    r"#sculptor\.deployment_route<"
    r"\s*id\s*=\s*(?P<route>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceCore\s*=\s*(?P<source_core>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceTask\s*=\s*(?P<source_task>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceOutput\s*=\s*(?P<source_output>[0-9]+)\s*:\s*i64\s*,"
    r"\s*destinationCore\s*=\s*(?P<destination_core>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*destinationTask\s*=\s*(?P<destination_task>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*destinationInput\s*=\s*(?P<destination_input>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*resourceId\s*=\s*(?P<resource>[0-9]+)\s*:\s*i64\s*,"
    r"\s*byteSize\s*=\s*(?P<byte_size>[0-9]+)\s*:\s*i64\s*>",
    re.MULTILINE,
)
TILE_ROUTINE_ROUTE_PATTERN = re.compile(
    r"#sculptor\.tile_routine_route<"
    r"\s*id\s*=\s*(?P<route>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceTile\s*=\s*(?P<source_core>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceRoutine\s*=\s*(?P<source_task>[0-9]+)\s*:\s*i64\s*,"
    r"\s*sourceOutput\s*=\s*(?P<source_output>[0-9]+)\s*:\s*i64\s*,"
    r"\s*destinationTile\s*=\s*(?P<destination_core>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*destinationRoutine\s*=\s*(?P<destination_task>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*destinationInput\s*=\s*(?P<destination_input>[0-9]+)"
    r"\s*:\s*i64\s*,"
    r"\s*resourceId\s*=\s*(?P<resource>[0-9]+)\s*:\s*i64\s*,"
    r"\s*tensorId\s*=\s*-?[0-9]+\s*:\s*i64\s*,"
    r"\s*byteSize\s*=\s*(?P<byte_size>[0-9]+)\s*:\s*i64\s*>",
    re.MULTILINE,
)
MESH_COLUMN_PATTERN = re.compile(
    r"sculptor\.schedule\.mesh_cols\s*=\s*([0-9]+)\s*:\s*i64"
)


def parse_routes(text):
    routes = {}
    for pattern in (ROUTE_PATTERN, TILE_ROUTINE_ROUTE_PATTERN):
        for match in pattern.finditer(text):
            route = {
                name: int(value)
                for name, value in match.groupdict().items()
            }
            route_id = route["route"]
            previous = routes.get(route_id)
            if previous is not None and previous != route:
                raise RuntimeError(
                    f"route {route_id} has conflicting deployment records"
                )
            routes[route_id] = route
    if not routes:
        raise RuntimeError("the MLIR contains no deployment routes")
    return [routes[route_id] for route_id in sorted(routes)]


def mesh_width(text, requested):
    if requested is not None:
        return requested
    match = MESH_COLUMN_PATTERN.search(text)
    return int(match.group(1)) if match is not None else None


def write_routes(path, routes, width):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "route_id",
                "source_core",
                "source_task",
                "source_output",
                "destination_core",
                "destination_task",
                "destination_input",
                "resource_id",
                "byte_size",
                "payload_words",
                "manhattan_hops",
            ]
        )
        for route in routes:
            if route["byte_size"] % 4 != 0:
                raise RuntimeError(
                    f"route {route['route']} byte size is not word aligned"
                )
            hops = ""
            if width is not None:
                source_x = route["source_core"] % width
                source_y = route["source_core"] // width
                destination_x = route["destination_core"] % width
                destination_y = route["destination_core"] // width
                hops = (
                    abs(source_x - destination_x)
                    + abs(source_y - destination_y)
                )
            writer.writerow(
                [
                    route["route"],
                    route["source_core"],
                    route["source_task"],
                    route["source_output"],
                    route["destination_core"],
                    route["destination_task"],
                    route["destination_input"],
                    route["resource"],
                    route["byte_size"],
                    route["byte_size"] // 4,
                    hops,
                ]
            )


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Extract the deduplicated Sculptor deployment route manifest "
            "needed by the Mittens critical-path profiler"
        )
    )
    parser.add_argument(
        "paths",
        type=Path,
        nargs="+",
        help=(
            "one or more partitioned/isolated MLIR inputs followed by "
            "the output CSV"
        ),
    )
    parser.add_argument("--mesh-width", type=int)
    args = parser.parse_args()

    if args.mesh_width is not None and args.mesh_width <= 0:
        raise RuntimeError("--mesh-width must be positive")
    if len(args.paths) < 2:
        raise RuntimeError(
            "provide at least one MLIR input and one output CSV"
        )
    input_paths = args.paths[:-1]
    output_csv = args.paths[-1]
    text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in input_paths
    )
    routes = parse_routes(text)
    width = mesh_width(text, args.mesh_width)
    write_routes(output_csv, routes, width)
    print(
        f"deployment route manifest: {len(routes)} routes"
        f" from {len(input_paths)} MLIR file(s)"
        + (f", mesh width {width}" if width is not None else "")
    )


if __name__ == "__main__":
    main()
