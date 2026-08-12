#!/usr/bin/env python3

import argparse
import csv
import json
import re
import shutil
import subprocess
from collections import defaultdict
from pathlib import Path


MAX_TICK = (1 << 64) - 1


INVALID_ROUTE_ID = 2**32 - 1
INVALID_TASK_ID = 2**32 - 1
LOCAL_ROUTER_PORT = "port4"
MITTENS_LOCAL_ROUTER_PORT = "local"
FREQUENCY_PATTERN = re.compile(
    r"^(?P<value>[0-9]+(?:\.[0-9]+)?)"
    r"(?P<unit>Hz|kHz|MHz|GHz)$"
)


def parse_frequency(value):
    match = FREQUENCY_PATTERN.fullmatch(value)
    if match is None:
        raise argparse.ArgumentTypeError(
            "frequency must use Hz, kHz, MHz, or GHz"
        )
    scale = {
        "Hz": 1.0,
        "kHz": 1e3,
        "MHz": 1e6,
        "GHz": 1e9,
    }[match.group("unit")]
    return float(match.group("value")) * scale


def read_csv_files(paths):
    rows = []
    for path in sorted(paths):
        with path.open("r", encoding="utf-8", newline="") as source:
            rows.extend(csv.DictReader(source))
    return rows


def integer(row, name, default=0):
    value = row.get(name, "")
    return default if value in {"", None} else int(value)


def optional_integer(row, name):
    value = row.get(name, "")
    return None if value in {"", None} else int(value)


def parse_task_trace(directory):
    paths = [
        path
        for path in directory.glob("tile-*.csv")
        if path.stem.removeprefix("tile-").isdigit()
    ]
    rows = read_csv_files(paths)
    active = {}
    tasks = []
    for row in sorted(
        rows,
        key=lambda item: (
            integer(item, "sim_time_ticks"),
            integer(item, "tile_id"),
            0 if item["event"] == "start" else 1,
        ),
    ):
        key = (
            integer(row, "execution_id"),
            integer(row, "task_id"),
        )
        if row["event"] == "start":
            if key in active:
                raise RuntimeError(f"duplicate task start for {key}")
            active[key] = row
            continue
        start = active.pop(key, None)
        if start is None:
            raise RuntimeError(f"task finish without start for {key}")
        if integer(start, "tile_id") != integer(row, "tile_id"):
            raise RuntimeError(f"task {key} changed tiles")
        start_tick = integer(start, "sim_time_ticks")
        finish_tick = integer(row, "sim_time_ticks")
        if finish_tick < start_tick:
            raise RuntimeError(f"negative task duration for {key}")
        start_instructions = integer(
            start, "retired_instructions")
        finish_instructions = integer(
            row, "retired_instructions")
        start_cycles = integer(start, "cpu_cycles")
        finish_cycles = integer(row, "cpu_cycles")
        if (
            finish_instructions < start_instructions
            or finish_cycles < start_cycles
        ):
            raise RuntimeError(
                f"negative task instruction interval for {key}"
            )
        tasks.append(
            {
                "execution_id": key[0],
                "task_id": key[1],
                "tile_id": integer(row, "tile_id"),
                "start_tick": start_tick,
                "finish_tick": finish_tick,
                "duration_ticks": finish_tick - start_tick,
                "retired_instructions":
                    finish_instructions - start_instructions,
                "cpu_cycles": finish_cycles - start_cycles,
            }
        )
    if active:
        raise RuntimeError(f"unfinished task traces: {sorted(active)}")
    return tasks


def load_route_manifest(path):
    if path is None:
        return {}
    routes = {}
    with path.open("r", encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            route_id = integer(row, "route_id")
            if route_id in routes:
                raise RuntimeError(
                    f"route manifest repeats route {route_id}"
                )
            route = {
                name: integer(row, name)
                for name in (
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
                )
            }
            route["manhattan_hops"] = optional_integer(
                row, "manhattan_hops"
            )
            routes[route_id] = route
    return routes


def join_network(rows):
    packets = {}
    for row in rows:
        source = integer(row, "source")
        packet_id = integer(row, "packet_id")
        key = (source, packet_id)
        packet = packets.setdefault(
            key,
            {
                "source": source,
                "packet_id": packet_id,
                "destination": integer(row, "destination"),
                "route_id": integer(row, "route_id"),
                "execution_id": integer(row, "execution_id"),
                "kind": row["kind"],
                "words": integer(row, "words"),
                "protocol_words": integer(row, "protocol_words"),
                "payload_words": integer(row, "payload_words"),
                "hops": integer(row, "hops"),
                "word_hops": integer(row, "word_hops"),
                "ready_tick": integer(row, "ready_tick"),
                "injection_tick": integer(row, "injection_tick"),
                "arrival_tick": None,
                "endpoint_queue_ticks": integer(
                    row, "endpoint_queue_ticks"
                ),
                "injection_seen": False,
            },
        )
        for name in (
            "destination",
            "route_id",
            "execution_id",
            "words",
            "protocol_words",
            "payload_words",
            "hops",
            "word_hops",
            "ready_tick",
            "injection_tick",
        ):
            if packet[name] != integer(row, name):
                raise RuntimeError(
                    f"network packet {key} changed field {name}"
                )
        if packet["kind"] != row["kind"]:
            raise RuntimeError(
                f"network packet {key} changed field kind"
            )
        if row["event"] == "arrive":
            arrival = integer(row, "event_tick")
            if packet["arrival_tick"] is not None:
                raise RuntimeError(
                    f"network packet {key} arrived more than once"
                )
            packet["arrival_tick"] = arrival
        elif row["event"] == "inject":
            if packet["injection_seen"]:
                raise RuntimeError(
                    f"network packet {key} was injected more than once"
                )
            packet["injection_seen"] = True
        else:
            raise RuntimeError(
                f"network packet {key} has unknown event {row['event']}"
            )
    completed = []
    for key, packet in sorted(packets.items()):
        if not packet.pop("injection_seen"):
            raise RuntimeError(f"network packet {key} was never injected")
        if packet["arrival_tick"] is None:
            raise RuntimeError(f"network packet {key} never arrived")
        packet["transit_ticks"] = (
            packet["arrival_tick"] - packet["injection_tick"]
        )
        if packet["transit_ticks"] < 0:
            raise RuntimeError(f"network packet {key} traveled backwards")
        completed.append(packet)
    return completed


def join_dma(rows):
    transfers = {}
    for row in rows:
        key = (
            integer(row, "tile_id"),
            integer(row, "source"),
            integer(row, "route_id"),
            integer(row, "execution_id"),
            integer(row, "burst_index"),
        )
        transfer = transfers.setdefault(
            key,
            {
                "tile_id": key[0],
                "source": key[1],
                "route_id": key[2],
                "execution_id": key[3],
                "burst_index": key[4],
                "words": integer(row, "words"),
                "schedule_tick": integer(row, "schedule_tick"),
                "dma_start_cycle": integer(row, "dma_start_cycle"),
                "dma_completion_cycle": integer(
                    row, "dma_completion_cycle"
                ),
                "service_cycles": integer(row, "service_cycles"),
                "invalidation_lines": integer(row, "invalidation_lines"),
                "completion_tick": None,
            },
        )
        if row["event"] == "complete":
            transfer["completion_tick"] = integer(row, "event_tick")
            transfer["invalidation_lines"] = integer(
                row, "invalidation_lines"
            )
    completed = []
    for key, transfer in sorted(transfers.items()):
        if transfer["completion_tick"] is None:
            raise RuntimeError(f"DMA transfer {key} never completed")
        completed.append(transfer)
    return completed


def aggregate_routes(packets, dma, manifest):
    routes = {}
    for packet in packets:
        route_id = packet["route_id"]
        if route_id == INVALID_ROUTE_ID:
            continue
        key = (
            packet["execution_id"],
            route_id,
            packet["source"],
            packet["destination"],
        )
        route = routes.setdefault(
            key,
            {
                "execution_id": key[0],
                "route_id": key[1],
                "source_core": key[2],
                "destination_core": key[3],
                "protocol_words": 0,
                "payload_words": 0,
                "word_hops": 0,
                "manhattan_hops": packet["hops"],
                "injection_start_tick": packet["injection_tick"],
                "injection_finish_tick": packet["injection_tick"],
                "arrival_start_tick": packet["arrival_tick"],
                "arrival_finish_tick": packet["arrival_tick"],
                "dma_finish_tick": None,
            },
        )
        route["protocol_words"] += packet["protocol_words"]
        route["payload_words"] += packet["payload_words"]
        route["word_hops"] += packet["word_hops"]
        if route["manhattan_hops"] != packet["hops"]:
            raise RuntimeError(
                f"route {key} changed Manhattan distance"
            )
        route["injection_start_tick"] = min(
            route["injection_start_tick"], packet["injection_tick"]
        )
        route["injection_finish_tick"] = max(
            route["injection_finish_tick"], packet["injection_tick"]
        )
        route["arrival_start_tick"] = min(
            route["arrival_start_tick"], packet["arrival_tick"]
        )
        route["arrival_finish_tick"] = max(
            route["arrival_finish_tick"], packet["arrival_tick"]
        )

    for transfer in dma:
        key = (
            transfer["execution_id"],
            transfer["route_id"],
            transfer["source"],
            transfer["tile_id"],
        )
        route = routes.get(key)
        if route is None:
            continue
        route["dma_finish_tick"] = max(
            route["dma_finish_tick"] or 0,
            transfer["completion_tick"],
        )

    completed = []
    for key, route in sorted(routes.items()):
        if manifest and route["route_id"] not in manifest:
            raise RuntimeError(
                f"observed route {route['route_id']} is absent from "
                "the deployment manifest"
            )
        definition = manifest.get(route["route_id"], {})
        for observed, expected in (
            ("source_core", "source_core"),
            ("destination_core", "destination_core"),
            ("manhattan_hops", "manhattan_hops"),
        ):
            if (
                definition.get(expected) is not None
                and route[observed] != definition[expected]
            ):
                raise RuntimeError(
                    f"route {route['route_id']} {observed} mismatch: "
                    f"observed {route[observed]}, expected "
                    f"{definition[expected]}"
                )
        for name in (
            "source_task",
            "destination_task",
            "resource_id",
            "byte_size",
        ):
            route[name] = definition.get(name)
        route["ready_tick"] = (
            route["dma_finish_tick"]
            if route["dma_finish_tick"] is not None
            else route["arrival_finish_tick"]
        )
        route["network_span_ticks"] = (
            route["arrival_finish_tick"]
            - route["injection_start_tick"]
        )
        route["end_to_end_ticks"] = (
            route["ready_tick"] - route["injection_start_tick"]
        )
        expected_words = definition.get("payload_words")
        route["payload_matches_manifest"] = (
            expected_words is None
            or expected_words == route["payload_words"]
        )
        if not route["payload_matches_manifest"]:
            raise RuntimeError(
                f"route {route['route_id']} carried "
                f"{route['payload_words']} payload words; expected "
                f"{expected_words}"
            )
        completed.append(route)
    return completed


def aggregate_analog(rows):
    operations = {}
    for row in rows:
        key = (integer(row, "tile_id"), integer(row, "ticket"))
        operation = operations.setdefault(
            key,
            {
                "tile_id": key[0],
                "ticket": key[1],
                "operation": integer(row, "operation"),
                "array_id": integer(row, "array_id"),
                "submitted_tick": None,
                "start_tick": None,
                "finish_tick": None,
                "queue_ticks": None,
                "input_transfer_ticks": 0,
                "compute_ticks": 0,
                "output_transfer_ticks": 0,
            },
        )
        phase = row["phase"]
        tick = integer(row, "event_tick")
        if phase == "submitted":
            operation["submitted_tick"] = tick
        elif phase.endswith("-start"):
            operation.setdefault("_starts", {})[phase[:-6]] = tick
            operation["start_tick"] = (
                tick
                if operation["start_tick"] is None
                else min(operation["start_tick"], tick)
            )
        elif phase.endswith("-finish"):
            base = phase[:-7]
            start = operation.setdefault("_starts", {}).get(base)
            if start is not None:
                duration = tick - start
                if "compute" in base:
                    operation["compute_ticks"] += duration
                elif "output" in base:
                    operation["output_transfer_ticks"] += duration
                else:
                    operation["input_transfer_ticks"] += duration
        elif phase == "complete":
            operation["finish_tick"] = tick

    completed = []
    for key, operation in sorted(operations.items()):
        operation.pop("_starts", None)
        if (
            operation["submitted_tick"] is None
            or operation["finish_tick"] is None
        ):
            raise RuntimeError(f"analog operation {key} is incomplete")
        if operation["start_tick"] is None:
            operation["start_tick"] = operation["submitted_tick"]
        operation["queue_ticks"] = (
            operation["start_tick"] - operation["submitted_tick"]
        )
        operation["total_ticks"] = (
            operation["finish_tick"] - operation["submitted_tick"]
        )
        completed.append(operation)
    return completed


def aggregate_memory(rows):
    requests = {}
    for row in rows:
        key = (integer(row, "tile_id"), integer(row, "request_id"))
        request = requests.setdefault(
            key,
            {
                "tile_id": key[0],
                "request_id": key[1],
                "address": integer(row, "address"),
                "timing_address": integer(
                    row, "timing_address", integer(row, "address")
                ),
                "guest_pc": integer(row, "guest_pc"),
                "guest_ra": integer(row, "guest_ra"),
                "size": integer(row, "size"),
                "direction": row["direction"],
                "task_id": integer(row, "task_id", INVALID_TASK_ID),
                "execution_id": integer(row, "execution_id"),
                "phase": row.get("phase", "unknown"),
                "issue_tick": integer(row, "issue_tick"),
                "response_tick": None,
            },
        )
        if row["event"] == "response":
            request["response_tick"] = integer(row, "event_tick")
    completed = []
    for key, request in sorted(requests.items()):
        if request["response_tick"] is None:
            raise RuntimeError(f"memory request {key} is incomplete")
        request["latency_ticks"] = (
            request["response_tick"] - request["issue_tick"]
        )
        completed.append(request)
    return completed


def aggregate_memory_sites(memory):
    groups = {}
    for request in memory:
        key = (
            request["tile_id"],
            request["execution_id"],
            request["task_id"],
            request["phase"],
            request["guest_pc"],
            request["guest_ra"],
        )
        group = groups.setdefault(
            key,
            {
                "tile_id": key[0],
                "execution_id": key[1],
                "task_id": key[2],
                "phase": key[3],
                "guest_pc": key[4],
                "guest_ra": key[5],
                "requests": 0,
                "reads": 0,
                "writes": 0,
                "bytes": 0,
                "memory_stall_ticks": 0,
                "maximum_service_ticks": 0,
                "source_function": "",
                "source_location": "",
                "caller_function": "",
                "caller_location": "",
                "elf": "",
            },
        )
        group["requests"] += 1
        group["reads"] += request["direction"] == "read"
        group["writes"] += request["direction"] == "write"
        group["bytes"] += request["size"]
        group["memory_stall_ticks"] += request["latency_ticks"]
        group["maximum_service_ticks"] = max(
            group["maximum_service_ticks"], request["latency_ticks"]
        )
    return [groups[key] for key in sorted(groups)]


def aggregate_memory_caller_sites(memory, symbols):
    groups = {}
    previous_key = None
    for request in memory:
        symbol = symbols.get((request["tile_id"], request["guest_pc"]))
        callee_function = symbol[0] if symbol is not None else ""
        key = (
            request["tile_id"],
            request["execution_id"],
            request["task_id"],
            request["phase"],
            callee_function,
            request["guest_ra"],
        )
        group = groups.setdefault(
            key,
            {
                "tile_id": key[0],
                "execution_id": key[1],
                "task_id": key[2],
                "phase": key[3],
                "callee_function": key[4],
                "guest_ra": key[5],
                "invocations": 0,
                "requests": 0,
                "reads": 0,
                "writes": 0,
                "bytes": 0,
                "memory_stall_ticks": 0,
                "maximum_service_ticks": 0,
                "caller_function": "",
                "caller_location": "",
                "elf": "",
            },
        )
        if key != previous_key:
            group["invocations"] += 1
        previous_key = key
        group["requests"] += 1
        group["reads"] += request["direction"] == "read"
        group["writes"] += request["direction"] == "write"
        group["bytes"] += request["size"]
        group["memory_stall_ticks"] += request["latency_ticks"]
        group["maximum_service_ticks"] = max(
            group["maximum_service_ticks"], request["latency_ticks"]
        )
    for group in groups.values():
        caller = symbols.get((group["tile_id"], group["guest_ra"]))
        if caller is not None:
            (
                group["caller_function"],
                group["caller_location"],
                group["elf"],
            ) = caller
    return [groups[key] for key in sorted(groups)]


def tile_elf(elf_directory, tile_id):
    candidates = (
        elf_directory / f"core-{tile_id}.elf",
        elf_directory / f"tile{tile_id}.elf",
        elf_directory / f"tile-{tile_id}.elf",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def symbolize_memory_sites(sites, elf_directory, symbolizer):
    if elf_directory is None:
        return {}
    if symbolizer is None:
        found = shutil.which("llvm-symbolizer")
        if found is None:
            raise RuntimeError(
                "--elf-directory requires --symbolizer or llvm-symbolizer "
                "in PATH"
            )
        symbolizer = Path(found)
    if not symbolizer.is_file():
        raise RuntimeError(f"memory symbolizer does not exist: {symbolizer}")

    by_tile = defaultdict(set)
    for site in sites:
        if site["guest_pc"] != 0:
            by_tile[site["tile_id"]].add(site["guest_pc"])
        if site["guest_ra"] != 0:
            by_tile[site["tile_id"]].add(site["guest_ra"])

    symbols = {}
    for tile_id, addresses in sorted(by_tile.items()):
        elf = tile_elf(elf_directory, tile_id)
        if elf is None:
            continue
        ordered = sorted(addresses)
        result = subprocess.run(
            [
                str(symbolizer),
                f"--obj={elf}",
                "--no-inlines",
                "--functions=linkage",
                "--demangle",
                "--output-style=JSON",
            ],
            input="".join(f"0x{address:x}\n" for address in ordered),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        records = [
            json.loads(line)
            for line in result.stdout.splitlines()
            if line.strip()
        ]
        if len(records) != len(ordered):
            raise RuntimeError(
                f"symbolizer returned {len(records)} records for "
                f"{len(ordered)} addresses in {elf}"
            )
        for address, record in zip(ordered, records):
            frames = record.get("Symbol", [])
            frame = frames[0] if frames else {}
            filename = frame.get("FileName", "")
            line = int(frame.get("Line", 0))
            column = int(frame.get("Column", 0))
            location = filename
            if location and line:
                location += f":{line}"
                if column:
                    location += f":{column}"
            symbols[(tile_id, address)] = (
                frame.get("FunctionName", ""),
                location,
                str(elf),
            )

    for site in sites:
        symbol = symbols.get((site["tile_id"], site["guest_pc"]))
        if symbol is not None:
            site["source_function"], site["source_location"], site["elf"] = symbol
        caller = symbols.get((site["tile_id"], site["guest_ra"]))
        if caller is not None:
            site["caller_function"], site["caller_location"], _ = caller
    return symbols


def aggregate_task_memory(memory, tasks):
    task_durations = {
        (task["tile_id"], task["execution_id"], task["task_id"]):
            task["duration_ticks"]
        for task in tasks
    }
    groups = {}
    for request in memory:
        key = (
            request["tile_id"],
            request["execution_id"],
            request["task_id"],
            request["phase"],
        )
        group = groups.setdefault(
            key,
            {
                "tile_id": key[0],
                "execution_id": key[1],
                "task_id": key[2],
                "phase": key[3],
                "requests": 0,
                "reads": 0,
                "writes": 0,
                "bytes": 0,
                "memory_stall_ticks": 0,
                "maximum_service_ticks": 0,
            },
        )
        group["requests"] += 1
        group["reads"] += request["direction"] == "read"
        group["writes"] += request["direction"] == "write"
        group["bytes"] += request["size"]
        group["memory_stall_ticks"] += request["latency_ticks"]
        group["maximum_service_ticks"] = max(
            group["maximum_service_ticks"], request["latency_ticks"]
        )
    result = []
    for key, group in sorted(groups.items()):
        duration = task_durations.get(key[:3], 0)
        group["task_duration_ticks"] = duration
        group["memory_fraction"] = (
            group["memory_stall_ticks"] / duration
            if duration > 0
            else 0.0
        )
        result.append(group)
    return result


def router_summary(path, finish_tick, timebase_ps, link_width_bits,
                   link_clock_hz):
    if path is None or not path.exists():
        return {
            "physical_link_bits": 0,
            "router_stalls": 0,
            "output_credit_stall_cycles": 0,
            "switch_arbitration_stall_cycles": 0,
            "input_buffer_full_cycles": 0,
            "links": [],
        }
    links = defaultdict(
        lambda: {
            "bits": 0,
            "packets": 0,
            "stalls": 0,
            "output_credit_stall_cycles": 0,
            "switch_arbitration_stall_cycles": 0,
            "input_buffer_full_cycles": 0,
            "output_link_busy_cycles": 0,
        }
    )
    cycle_ticks = 1e12 / link_clock_hz / timebase_ps
    with path.open("r", encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            component = row["ComponentName"]
            port = row["StatisticSubId"]
            if (
                not component.startswith("router_") or
                port in {LOCAL_ROUTER_PORT, MITTENS_LOCAL_ROUTER_PORT}
            ):
                continue
            key = (component, port)
            value = int(row.get("Sum.u64", 0))
            if row["StatisticName"] == "send_bit_count":
                links[key]["bits"] += value
            elif row["StatisticName"] == "flits_forwarded":
                links[key]["bits"] += value * 32
            elif row["StatisticName"] == "send_packet_count":
                links[key]["packets"] += value
            elif row["StatisticName"] == "packets_forwarded":
                links[key]["packets"] += value
            elif row["StatisticName"] == "output_port_stalls":
                cycles = int(round(value / cycle_ticks))
                links[key]["stalls"] += cycles
                links[key]["output_credit_stall_cycles"] += cycles
            elif row["StatisticName"] == "xbar_stalls":
                links[key]["stalls"] += value
                links[key]["switch_arbitration_stall_cycles"] += value
            elif row["StatisticName"] == \
                    "output_credit_stall_cycles":
                links[key]["stalls"] += value
                links[key]["output_credit_stall_cycles"] += value
            elif row["StatisticName"] == \
                    "switch_arbitration_stall_cycles":
                links[key]["stalls"] += value
                links[key]["switch_arbitration_stall_cycles"] += value
            elif row["StatisticName"] == "input_buffer_full_cycles":
                links[key]["input_buffer_full_cycles"] += value
            elif row["StatisticName"] == "output_link_busy_cycles":
                links[key]["output_link_busy_cycles"] += value
    seconds = finish_tick * timebase_ps * 1e-12
    capacity = seconds * link_clock_hz * link_width_bits
    records = []
    for (component, port), values in sorted(links.items()):
        records.append(
            {
                "component": component,
                "port": port,
                **values,
                "utilization": (
                    values["bits"] / capacity if capacity > 0 else 0.0
                ),
            }
        )
    return {
        "physical_link_bits": sum(item["bits"] for item in records),
        "router_stalls": sum(item["stalls"] for item in records),
        "output_credit_stall_cycles": sum(
            item["output_credit_stall_cycles"] for item in records
        ),
        "switch_arbitration_stall_cycles": sum(
            item["switch_arbitration_stall_cycles"] for item in records
        ),
        "input_buffer_full_cycles": sum(
            item["input_buffer_full_cycles"] for item in records
        ),
        "links": records,
    }


def critical_chain(tasks, routes):
    if not tasks:
        return []
    task_by_key = {
        (task["execution_id"], task["task_id"]): task
        for task in tasks
    }
    previous_on_tile = {}
    for tile_tasks in group_by(tasks, "tile_id").values():
        ordered = sorted(
            tile_tasks,
            key=lambda task: (task["start_tick"], task["finish_tick"]),
        )
        for previous, current in zip(ordered, ordered[1:]):
            previous_on_tile[
                (current["execution_id"], current["task_id"])
            ] = previous

    incoming = defaultdict(list)
    for route in routes:
        if (
            route["source_task"] is None
            or route["destination_task"] is None
        ):
            continue
        incoming[
            (route["execution_id"], route["destination_task"])
        ].append(route)

    predecessor = {}
    for key, task in task_by_key.items():
        candidates = []
        core_previous = previous_on_tile.get(key)
        if core_previous is not None:
            candidates.append(
                (
                    core_previous["finish_tick"],
                    "core",
                    core_previous,
                    None,
                )
            )
        for route in incoming.get(key, []):
            source_task = task_by_key.get(
                (route["execution_id"], route["source_task"])
            )
            if source_task is not None:
                candidates.append(
                    (
                        route["ready_tick"],
                        "route",
                        source_task,
                        route,
                    )
                )
        eligible = [
            candidate
            for candidate in candidates
            if candidate[0] <= task["start_tick"]
        ]
        predecessor[key] = max(
            eligible,
            key=lambda candidate: candidate[0],
            default=None,
        )

    endpoint = max(tasks, key=lambda task: task["finish_tick"])
    chain = []
    visited = set()
    current = endpoint
    while current is not None:
        key = (current["execution_id"], current["task_id"])
        if key in visited:
            raise RuntimeError("critical-chain predecessor cycle")
        visited.add(key)
        candidate = predecessor.get(key)
        record = dict(current)
        if candidate is None:
            record.update(
                {
                    "predecessor_type": "root",
                    "predecessor_task": None,
                    "route_id": None,
                    "ready_tick": current["start_tick"],
                    "wait_ticks": 0,
                }
            )
            current = None
        else:
            ready_tick, kind, source_task, route = candidate
            record.update(
                {
                    "predecessor_type": kind,
                    "predecessor_task": source_task["task_id"],
                    "route_id": (
                        route["route_id"] if route is not None else None
                    ),
                    "ready_tick": ready_tick,
                    "wait_ticks": current["start_tick"] - ready_tick,
                }
            )
            current = source_task
        chain.append(record)
    chain.reverse()
    for index, record in enumerate(chain):
        record["critical_index"] = index
    return chain


def group_by(rows, name):
    groups = defaultdict(list)
    for row in rows:
        groups[row[name]].append(row)
    return groups


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=fieldnames, extrasaction="ignore"
        )
        writer.writeheader()
        writer.writerows(rows)


def load_tile_summaries(directory):
    totals = defaultdict(int)
    finish_tick = 0
    for path in sorted(directory.glob("tile-*-summary.csv")):
        values = {}
        with path.open("r", encoding="utf-8", newline="") as source:
            for row in csv.DictReader(source):
                values[row["metric"]] = int(row["value"])
        finish_tick = max(finish_tick, values.get("finish_tick", 0))
        for name, value in values.items():
            if name not in {"tile_id", "finish_tick"}:
                totals[name] += value
    return totals, finish_tick


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Join Mittens per-tile traces and reconstruct system-level "
            "performance, route, and critical-chain metrics"
        )
    )
    parser.add_argument("profile_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--router-statistics", type=Path)
    parser.add_argument("--task-trace-directory", type=Path)
    parser.add_argument("--route-manifest", type=Path)
    parser.add_argument("--elf-directory", type=Path)
    parser.add_argument("--symbolizer", type=Path)
    parser.add_argument("--timebase-ps", type=float, default=1.0)
    parser.add_argument("--mesh-link-width-bits", type=int, default=32)
    parser.add_argument(
        "--mesh-link-clock",
        type=parse_frequency,
        default=parse_frequency("1GHz"),
        metavar="FREQUENCY",
    )
    args = parser.parse_args()

    profile = args.profile_directory
    totals, finish_tick = load_tile_summaries(profile)
    if finish_tick == 0:
        raise RuntimeError("the profile contains no tile summaries")

    network = join_network(
        read_csv_files(profile.glob("tile-*-network.csv"))
    )
    dma = join_dma(
        read_csv_files(profile.glob("tile-*-receive-dma.csv"))
    )
    waits = read_csv_files(profile.glob("tile-*-waits.csv"))
    # SST calls Component::finish() at the maximum simulation tick after the
    # event queue drains.  A worker can still be blocked in its final receive
    # wait at that point.  Such an open interval is not simulated work and
    # must not turn the reported makespan into UINT64_MAX picoseconds.
    waits = [
        row for row in waits
        if integer(row, "finish_tick") != MAX_TICK
    ]
    transmit_blocked = read_csv_files(
        profile.glob("tile-*-transmit-blocked.csv")
    )
    analog = aggregate_analog(
        read_csv_files(profile.glob("tile-*-analog.csv"))
    )
    memory = aggregate_memory(
        read_csv_files(profile.glob("tile-*-memory.csv"))
    )
    memory_sites = aggregate_memory_sites(memory)
    memory_symbols = symbolize_memory_sites(
        memory_sites, args.elf_directory, args.symbolizer
    )
    memory_caller_sites = aggregate_memory_caller_sites(
        memory, memory_symbols
    )
    manifest = load_route_manifest(args.route_manifest)
    routes = aggregate_routes(network, dma, manifest)
    tasks = (
        parse_task_trace(args.task_trace_directory)
        if args.task_trace_directory is not None
        else []
    )
    finish_tick_reconstructed = finish_tick == MAX_TICK
    if finish_tick_reconstructed:
        finish_candidates = [
            task["finish_tick"] for task in tasks
        ]
        finish_candidates.extend(
            integer(row, "finish_tick") for row in waits
        )
        finish_candidates.extend(
            row["arrival_tick"] for row in network
        )
        finish_candidates.extend(
            row["finish_tick"] for row in analog
        )
        finish_candidates.extend(
            row["response_tick"] for row in memory
        )
        finish_candidates.extend(
            row["completion_tick"] for row in dma
        )
        if not finish_candidates:
            raise RuntimeError(
                "the profile ended at UINT64_MAX and has no finite events"
            )
        finish_tick = max(finish_candidates)
    task_memory = aggregate_task_memory(memory, tasks)
    critical = critical_chain(tasks, routes)
    routers = router_summary(
        args.router_statistics,
        finish_tick,
        args.timebase_ps,
        args.mesh_link_width_bits,
        args.mesh_link_clock,
    )

    output = args.output_directory
    write_csv(
        output / "network-packets.csv",
        network,
        list(network[0]) if network else ["source", "packet_id"],
    )
    write_csv(
        output / "routes.csv",
        routes,
        list(routes[0]) if routes else ["execution_id", "route_id"],
    )
    write_csv(
        output / "receive-dma.csv",
        dma,
        list(dma[0]) if dma else ["tile_id", "route_id"],
    )
    write_csv(
        output / "analog-operations.csv",
        analog,
        list(analog[0]) if analog else ["tile_id", "ticket"],
    )
    write_csv(
        output / "memory-requests.csv",
        memory,
        list(memory[0]) if memory else ["tile_id", "request_id"],
    )
    write_csv(
        output / "memory-sites.csv",
        memory_sites,
        (
            list(memory_sites[0])
            if memory_sites
            else [
                "tile_id",
                "execution_id",
                "task_id",
                "phase",
                "guest_pc",
                "guest_ra",
                "requests",
                "reads",
                "writes",
                "bytes",
                "memory_stall_ticks",
                "maximum_service_ticks",
                "source_function",
                "source_location",
                "caller_function",
                "caller_location",
                "elf",
            ]
        ),
    )
    write_csv(
        output / "memory-caller-sites.csv",
        memory_caller_sites,
        (
            list(memory_caller_sites[0])
            if memory_caller_sites
            else [
                "tile_id",
                "execution_id",
                "task_id",
                "phase",
                "callee_function",
                "guest_ra",
                "invocations",
                "requests",
                "reads",
                "writes",
                "bytes",
                "memory_stall_ticks",
                "maximum_service_ticks",
                "caller_function",
                "caller_location",
                "elf",
            ]
        ),
    )
    write_csv(
        output / "task-memory.csv",
        task_memory,
        (
            list(task_memory[0])
            if task_memory
            else ["tile_id", "execution_id", "task_id", "phase"]
        ),
    )
    write_csv(
        output / "waits.csv",
        waits,
        list(waits[0]) if waits else ["tile_id", "reason"],
    )
    write_csv(
        output / "transmit-blocked.csv",
        transmit_blocked,
        (
            list(transmit_blocked[0])
            if transmit_blocked
            else ["tile_id", "route_id", "duration_ticks"]
        ),
    )
    write_csv(
        output / "link-statistics.csv",
        routers["links"],
        [
            "component",
            "port",
            "bits",
            "packets",
            "stalls",
            "output_credit_stall_cycles",
            "switch_arbitration_stall_cycles",
            "input_buffer_full_cycles",
            "output_link_busy_cycles",
            "utilization",
        ],
    )
    write_csv(
        output / "tasks.csv",
        tasks,
        (
            list(tasks[0])
            if tasks
            else ["execution_id", "task_id", "tile_id"]
        ),
    )
    write_csv(
        output / "critical-path.csv",
        critical,
        (
            list(critical[0])
            if critical
            else ["critical_index", "execution_id", "task_id"]
        ),
    )

    wait_by_reason = defaultdict(int)
    for row in waits:
        wait_by_reason[row["reason"]] += integer(row, "duration_ticks")
    if waits:
        for name in list(totals):
            if name.startswith("wait_") and name.endswith("_ticks"):
                totals[name] = 0
        for reason, value in wait_by_reason.items():
            totals[f"wait_{reason}_ticks"] = value
    if not waits:
        for name, value in totals.items():
            if name.startswith("wait_") and name.endswith("_ticks"):
                reason = name[len("wait_"):-len("_ticks")]
                if reason != "none" and value != 0:
                    wait_by_reason[reason] += value
    network_packets = (
        len(network)
        if network
        else totals.get("network_packets", 0)
    )
    injected_words = (
        sum(row["words"] for row in network)
        if network
        else totals.get("network_words", 0)
    )
    directional_word_hops = (
        sum(row["word_hops"] for row in network)
        if network
        else totals.get("network_word_hops", 0)
    )
    packet_transit_ticks = (
        sum(row["transit_ticks"] for row in network)
        if network
        else totals.get("network_transit_ticks", 0)
    )
    endpoint_queue_ticks = (
        sum(row["endpoint_queue_ticks"] for row in network)
        if network
        else totals.get("network_endpoint_queue_ticks", 0)
    )
    task_instructions = sum(
        task["retired_instructions"] for task in tasks)
    task_cpu_cycles = sum(
        task["cpu_cycles"] for task in tasks)
    outside_task_instructions = max(
        totals.get("instructions", 0) - task_instructions,
        0,
    )
    summary = {
        "schema_version": 2,
        "timebase_ps": args.timebase_ps,
        "finish_tick": finish_tick,
        "finish_tick_reconstructed": finish_tick_reconstructed,
        "simulated_time_seconds": finish_tick * args.timebase_ps * 1e-12,
        "tile_totals": dict(sorted(totals.items())),
        "network": {
            "packets": network_packets,
            "injected_words": injected_words,
            "protocol_words": sum(
                row["protocol_words"] for row in network
            ),
            "payload_words": sum(
                row["payload_words"] for row in network
            ),
            "directional_word_hops": directional_word_hops,
            "packet_transit_ticks": packet_transit_ticks,
            "endpoint_queue_ticks": endpoint_queue_ticks,
            "physical_router_link_bits": routers[
                "physical_link_bits"
            ],
            "router_stalls": routers["router_stalls"],
            "output_credit_stall_cycles": routers[
                "output_credit_stall_cycles"
            ],
            "switch_arbitration_stall_cycles": routers[
                "switch_arbitration_stall_cycles"
            ],
            "input_buffer_full_cycles": routers[
                "input_buffer_full_cycles"
            ],
            "routes": len(routes),
            "packet_detail_available": bool(network),
        },
        "receive_dma": {
            "transfers": len(dma),
            "words": sum(row["words"] for row in dma),
            "active_cycles": (
                sum(row["service_cycles"] for row in dma)
                if dma
                else totals.get("receive_dma_active_cycles", 0)
            ),
            "detail_available": bool(dma),
        },
        "analog": {
            "operations": len(analog),
            "queue_ticks": sum(row["queue_ticks"] for row in analog),
            "total_operation_ticks": sum(
                row["total_ticks"] for row in analog
            ),
            "active_cycles": totals.get("analog_active_cycles", 0),
            "link_beats": totals.get("analog_link_beats", 0),
            "detail_available": bool(analog),
        },
        "memory": {
            "modeled_requests": len(memory),
            "task_attributed_requests": sum(
                row["task_id"] != INVALID_TASK_ID for row in memory
            ),
            "task_groups": len(task_memory),
            "total_latency_ticks": sum(
                row["latency_ticks"] for row in memory
            ),
            "modeled": bool(memory),
        },
        "transmit_backpressure": {
            "blocked_ticks": (
                sum(
                    integer(row, "duration_ticks")
                    for row in transmit_blocked
                )
                if transmit_blocked
                else totals.get("transmit_blocked_ticks", 0)
            ),
            "events": (
                len(transmit_blocked)
                if transmit_blocked
                else totals.get("transmit_blocked_events", 0)
            ),
            "retries": (
                sum(
                    integer(row, "retry_count")
                    for row in transmit_blocked
                )
                if transmit_blocked
                else totals.get("transmit_blocked_retries", 0)
            ),
            "maximum_queue_occupancy": max(
                (
                    integer(row, "maximum_queue_occupancy")
                    for row in transmit_blocked
                ),
                default=totals.get(
                    "transmit_maximum_queue_occupancy", 0
                ),
            ),
            "detail_available": bool(transmit_blocked),
        },
        "wait_ticks_by_reason": dict(sorted(wait_by_reason.items())),
        "tasks": {
            "count": len(tasks),
            "critical_chain_tasks": len(critical),
            "critical_chain_available": bool(critical),
            "retired_instructions": task_instructions,
            "cpu_cycles": task_cpu_cycles,
            "outside_task_instructions":
                outside_task_instructions,
        },
    }
    output.mkdir(parents=True, exist_ok=True)
    with (output / "summary.json").open(
        "w", encoding="utf-8"
    ) as summary_file:
        json.dump(summary, summary_file, indent=2, sort_keys=True)
        summary_file.write("\n")
    with (output / "summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as summary_file:
        writer = csv.writer(summary_file)
        writer.writerow(["metric", "value"])
        writer.writerow(["simulated_time_seconds", summary[
            "simulated_time_seconds"
        ]])
        writer.writerow(["network_packets", network_packets])
        writer.writerow(["injected_words", injected_words])
        writer.writerow(["directional_word_hops", summary["network"][
            "directional_word_hops"
        ]])
        writer.writerow(["router_stalls", routers["router_stalls"]])
        writer.writerow(["analog_operations", len(analog)])
        writer.writerow(["memory_requests", len(memory)])
        writer.writerow(
            [
                "transmit_blocked_ticks",
                summary["transmit_backpressure"]["blocked_ticks"],
            ]
        )
        writer.writerow(
            [
                "transmit_blocked_events",
                summary["transmit_backpressure"]["events"],
            ]
        )
        writer.writerow(["task_count", len(tasks)])
        writer.writerow(["critical_chain_tasks", len(critical)])

    print(
        "Mittens profile: "
        f"{summary['simulated_time_seconds']:.9f} s, "
        f"{len(tasks)} tasks, "
        f"{len(routes)} routes, "
        f"{injected_words} injected words, "
        f"{directional_word_hops} word-hops"
    )


if __name__ == "__main__":
    main()
