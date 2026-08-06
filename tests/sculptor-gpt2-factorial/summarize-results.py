#!/usr/bin/env python3
"""Create one stable CSV row for every GPT-2 factorial trial."""

from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path
from typing import Any


TOKEN_COUNTS = (4, 8, 16, 32)
FACTORS = (
    "boundary_regret",
    "compact_region",
    "spatial_link_pressure",
    "balanced_reductions",
    "distributed_matmul",
)


def configurations() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for boundary_regret in (0, 1):
        for compact_region in (0, 1):
            for link_pressure in (0, 1):
                for balanced_reductions in (0, 1):
                    for distributed_matmul in (0, 1):
                        name = (
                            f"br{boundary_regret}-cr{compact_region}-"
                            f"lp{link_pressure}-rb{balanced_reductions}-"
                            f"dm{distributed_matmul}"
                        )
                        terms = ["transfer-cost"]
                        if boundary_regret:
                            terms.append("boundary-regret")
                        if compact_region:
                            terms.append("compact-region")
                        if link_pressure:
                            terms.append("spatial-link-pressure")
                        terms.extend(
                            ("lookahead=3", "beam=8", "scope=diagonal")
                        )
                        rows.append(
                            {
                                "configuration": name,
                                "boundary_regret": boundary_regret,
                                "compact_region": compact_region,
                                "spatial_link_pressure": link_pressure,
                                "balanced_reductions": balanced_reductions,
                                "distributed_matmul": distributed_matmul,
                                "greedy_heuristic": ",".join(terms),
                            }
                        )
    return rows


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object in {path}")
    return value


def nested(mapping: dict[str, Any], *keys: str) -> Any:
    value: Any = mapping
    for key in keys:
        if not isinstance(value, dict):
            return ""
        value = value.get(key, "")
    return value


def scheduler_fields(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    if len(rows) != 1:
        raise ValueError(f"expected one scheduler row in {path}")
    row = rows[0]

    def field(index: int) -> str:
        return row[index] if index < len(row) else ""

    return {
        "task_count": field(10),
        "dependency_count": field(11),
        "logical_arrays": field(12),
        "total_digital_ops": field(13),
        "inter_core_transfer_bytes": field(14),
        "total_transfer_cost": field(15),
        "transfer_cost_per_byte": field(16),
        "boundary_penalty": field(17),
        "graph_score": field(18),
        "placement_cost_mode": field(26),
        "search_completion_proxy": field(27),
        "search_communication_proxy": field(28),
        "search_resource_load_proxy": field(29),
        "predicted_makespan_ns": field(30),
        "predicted_exposed_contention_ns": field(31),
        "predicted_exposed_transport_ns": field(32),
        "predicted_total_word_hops": field(33),
        "timing_rerank_candidate_count": field(34),
        "timing_rerank_selected_proxy_rank": field(35),
    }


def route_fields(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as stream:
        routes = list(csv.DictReader(stream))
    return {
        "route_count": len(routes),
        "route_bytes": sum(int(row["byte_size"]) for row in routes),
        "route_payload_words": sum(
            int(row["payload_words"]) for row in routes
        ),
        "route_word_hops": sum(
            int(row["payload_words"]) * int(row["manhattan_hops"])
            for row in routes
        ),
    }


def simulation_time_from_log(path: Path) -> str:
    if not path.is_file():
        return ""
    pattern = re.compile(
        r"Simulation is complete, simulated time:\s*"
        r"([0-9.eE+-]+)\s*(s|ms|us|ns|ps)"
    )
    scale = {
        "s": 1.0e9,
        "ms": 1.0e6,
        "us": 1.0e3,
        "ns": 1.0,
        "ps": 1.0e-3,
    }
    match = None
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            candidate = pattern.search(line)
            if candidate:
                match = candidate
    if match is None:
        return ""
    return f"{float(match.group(1)) * scale[match.group(2)]:.0f}"


def output_signature(path: Path) -> dict[str, str]:
    values = {
        "output_elements": "",
        "finite_elements": "",
        "first_bits": "",
        "checksum_bits": "",
    }
    if not path.is_file():
        return values
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "GPT2_OUTPUT " not in line:
                continue
            fields = dict(re.findall(r"([a-z_]+)=([^\s\r]+)", line))
            values = {
                "output_elements": fields.get("elements", ""),
                "finite_elements": fields.get("finite", ""),
                "first_bits": fields.get("first_bits", ""),
                "checksum_bits": fields.get("checksum_bits", ""),
            }
    return values


def status_for(configuration: Path) -> str:
    deployment = configuration / "deployment"
    if (deployment / "status.pass").is_file():
        return "pass"
    if (deployment / "status.failed").is_file():
        return "failed"
    if (configuration / ".complete").is_file():
        return "built"
    return "not-built"


def active_core_count(path: Path) -> str:
    if not path.is_file():
        return ""
    return str(sum(1 for line in path.read_text().splitlines() if line.strip()))


def make_row(
    output_root: Path, token_count: int, configuration: dict[str, Any]
) -> dict[str, Any]:
    name = configuration["configuration"]
    directory = (
        output_root
        / f"tokens-{token_count}"
        / "configurations"
        / name
    )
    deployment = directory / "deployment"
    profile_path = deployment / "performance-profile" / "summary.json"
    profile = load_json(profile_path)
    simulation_log = deployment / "simulation.log"
    simulated_time_ns: Any = ""
    if profile.get("simulated_time_seconds") is not None:
        simulated_time_ns = round(
            float(profile["simulated_time_seconds"]) * 1.0e9
        )
    else:
        simulated_time_ns = simulation_time_from_log(simulation_log)

    row: dict[str, Any] = {
        "tokens": token_count,
        **configuration,
        "schedule": "greedy-timing",
        "lookahead": 3,
        "beam_width": 8,
        "candidate_scope": "diagonal",
        "transfer_cost_baseline": 1,
        "reduction_width": 2,
        "matmul_max_shards": 8,
        "matmul_min_ops_per_shard": 1,
        "matmul_placement_policy": "prefer-distinct",
        "mesh_width": 12,
        "mesh_height": 12,
        "arrays_per_core": 4,
        "array_rows": 1024,
        "array_columns": 512,
        "cpu_issue_width": 2,
        "cpu_clock_ghz": 1,
        "analog_mvm_latency_ns": 100,
        "memory_backend": "native",
        "transmit_policy": "async",
        "status": status_for(directory),
        "active_cores": active_core_count(directory / "active-cores.txt"),
        "simulated_time_ns": simulated_time_ns,
        **scheduler_fields(directory / "scheduler-summary.csv"),
        **route_fields(directory / "deployment-routes.csv"),
        **output_signature(simulation_log),
        "instructions": nested(profile, "tile_totals", "instructions"),
        "vector_instructions": nested(
            profile, "tile_totals", "vector_instructions"
        ),
        "cpu_cycles": nested(profile, "tile_totals", "cpu_cycles"),
        "task_instructions": nested(
            profile, "tasks", "retired_instructions"
        ),
        "outside_task_instructions": nested(
            profile, "tasks", "outside_task_instructions"
        ),
        "executed_tasks": nested(profile, "tasks", "count"),
        "network_words": nested(profile, "network", "injected_words"),
        "network_payload_words": nested(
            profile, "network", "payload_words"
        ),
        "network_protocol_words": nested(
            profile, "network", "protocol_words"
        ),
        "network_word_hops": nested(
            profile, "network", "directional_word_hops"
        ),
        "network_packets": nested(profile, "network", "packets"),
        "network_router_stalls": nested(
            profile, "network", "router_stalls"
        ),
        "network_endpoint_queue_ticks": nested(
            profile, "network", "endpoint_queue_ticks"
        ),
        "network_transit_ticks": nested(
            profile, "network", "packet_transit_ticks"
        ),
        "transmit_blocked_ticks": nested(
            profile, "transmit_backpressure", "blocked_ticks"
        ),
        "transmit_blocked_events": nested(
            profile, "transmit_backpressure", "events"
        ),
        "analog_active_cycles": nested(profile, "analog", "active_cycles"),
        "analog_link_beats": nested(profile, "analog", "link_beats"),
        "analog_operations": nested(profile, "analog", "operations"),
        "analog_queue_ticks": nested(profile, "analog", "queue_ticks"),
        "receive_dma_active_cycles": nested(
            profile, "receive_dma", "active_cycles"
        ),
        "wait_nic_receive_ticks": nested(
            profile, "wait_ticks_by_reason", "nic-receive-wait"
        ),
        "wait_nic_transmit_ticks": nested(
            profile, "wait_ticks_by_reason", "nic-transmit"
        ),
        "wait_analog_submit_ticks": nested(
            profile, "wait_ticks_by_reason", "analog-submit"
        ),
        "scheduler_summary": str(directory / "scheduler-summary.csv"),
        "task_core_map": str(directory / "task-core-map.csv"),
        "route_manifest": str(directory / "deployment-routes.csv"),
        "profile_summary": str(profile_path),
        "simulation_log": str(simulation_log),
    }
    return row


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: summarize-results.py OUTPUT_ROOT RESULTS_CSV"
        )
    output_root = Path(sys.argv[1]).resolve()
    results_path = Path(sys.argv[2]).resolve()
    rows = [
        make_row(output_root, tokens, configuration)
        for tokens in TOKEN_COUNTS
        for configuration in configurations()
    ]
    results_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = results_path.with_suffix(results_path.suffix + ".tmp")
    with temporary.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(results_path)

    counts = {
        status: sum(row["status"] == status for row in rows)
        for status in ("pass", "built", "failed", "not-built")
    }
    print(
        "GPT-2 factorial results: "
        f"{counts['pass']} passed, {counts['built']} built, "
        f"{counts['failed']} failed, {counts['not-built']} not built, "
        f"{len(rows)} total"
    )
    print(f"GPT-2 factorial CSV: {results_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
