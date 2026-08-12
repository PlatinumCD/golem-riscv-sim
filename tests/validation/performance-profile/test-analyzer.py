#!/usr/bin/env python3

import csv
import json
import subprocess
import tempfile
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parents[2]
ANALYZER = PROJECT_ROOT / "scripts" / "analyze-performance-profile.py"
ROUTE_EXTRACTOR = (
    PROJECT_ROOT / "scripts" / "extract-deployment-routes.py"
)


def write_csv(path, fieldnames, rows):
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_summary(
    path,
    tile_id,
    finish_tick,
    network_packets,
    network_words,
    network_word_hops,
):
    write_csv(
        path,
        ["metric", "value"],
        [
            {"metric": "tile_id", "value": tile_id},
            {"metric": "finish_tick", "value": finish_tick},
            {"metric": "instructions", "value": 100},
            {"metric": "cpu_cycles", "value": 50},
            {"metric": "network_packets", "value": network_packets},
            {"metric": "network_words", "value": network_words},
            {
                "metric": "network_word_hops",
                "value": network_word_hops,
            },
            {
                "metric": "wait_nic-receive-wait_ticks",
                "value": 25 if tile_id == 1 else 0,
            },
            {
                "metric": "transmit_blocked_ticks",
                "value": 15 if tile_id == 0 else 0,
            },
            {
                "metric": "transmit_blocked_events",
                "value": 1 if tile_id == 0 else 0,
            },
            {
                "metric": "transmit_blocked_retries",
                "value": 3 if tile_id == 0 else 0,
            },
            {
                "metric": "transmit_maximum_queue_occupancy",
                "value": 5 if tile_id == 0 else 0,
            },
        ],
    )


def main():
    with tempfile.TemporaryDirectory(
        prefix="mittens-profile-analysis-"
    ) as raw_directory:
        root = Path(raw_directory)
        profile = root / "raw"
        report = root / "report"
        profile.mkdir()

        write_summary(
            profile / "tile-0-summary.csv", 0, 800, 2, 9, 9
        )
        write_summary(
            profile / "tile-1-summary.csv", 1, 900, 0, 0, 0
        )

        network_fields = [
            "tile_id",
            "event",
            "packet_id",
            "source",
            "destination",
            "route_id",
            "execution_id",
            "kind",
            "words",
            "protocol_words",
            "payload_words",
            "hops",
            "word_hops",
            "ready_tick",
            "injection_tick",
            "event_tick",
            "endpoint_queue_ticks",
            "transit_ticks",
        ]
        packets = [
            {
                "packet_id": 0,
                "kind": "frame-header",
                "words": 5,
                "protocol_words": 5,
                "payload_words": 0,
                "injection_tick": 200,
                "arrival_tick": 300,
            },
            {
                "packet_id": 1,
                "kind": "frame-payload",
                "words": 4,
                "protocol_words": 0,
                "payload_words": 4,
                "injection_tick": 210,
                "arrival_tick": 310,
            },
        ]
        network_rows = []
        for packet in packets:
            common = {
                "source": 0,
                "destination": 1,
                "route_id": 7,
                "execution_id": 0,
                "hops": 1,
                "word_hops": packet["words"],
                "ready_tick": 190,
                "endpoint_queue_ticks": packet["injection_tick"] - 190,
                **{
                    name: packet[name]
                    for name in (
                        "packet_id",
                        "kind",
                        "words",
                        "protocol_words",
                        "payload_words",
                        "injection_tick",
                    )
                },
            }
            network_rows.append(
                {
                    "tile_id": 0,
                    "event": "inject",
                    "event_tick": packet["injection_tick"],
                    "transit_ticks": 0,
                    **common,
                }
            )
            network_rows.append(
                {
                    "tile_id": 1,
                    "event": "arrive",
                    "event_tick": packet["arrival_tick"],
                    "transit_ticks": (
                        packet["arrival_tick"]
                        - packet["injection_tick"]
                    ),
                    **common,
                }
            )
        write_csv(
            profile / "tile-0-network.csv",
            network_fields,
            network_rows[:1] + network_rows[2:3],
        )
        write_csv(
            profile / "tile-0-transmit-blocked.csv",
            [
                "tile_id",
                "event_sequence",
                "route_id",
                "execution_id",
                "destination",
                "kind",
                "words",
                "start_tick",
                "finish_tick",
                "duration_ticks",
                "retry_count",
                "maximum_queue_occupancy",
            ],
            [
                {
                    "tile_id": 0,
                    "event_sequence": 1,
                    "route_id": 7,
                    "execution_id": 0,
                    "destination": 1,
                    "kind": "frame-payload",
                    "words": 4,
                    "start_tick": 190,
                    "finish_tick": 205,
                    "duration_ticks": 15,
                    "retry_count": 3,
                    "maximum_queue_occupancy": 5,
                }
            ],
        )
        write_csv(
            profile / "tile-1-network.csv",
            network_fields,
            network_rows[1:2] + network_rows[3:],
        )

        dma_fields = [
            "tile_id",
            "event",
            "source",
            "route_id",
            "execution_id",
            "burst_index",
            "words",
            "schedule_tick",
            "dma_start_cycle",
            "dma_completion_cycle",
            "event_tick",
            "service_cycles",
        ]
        write_csv(
            profile / "tile-1-receive-dma.csv",
            dma_fields,
            [
                {
                    "tile_id": 1,
                    "event": "schedule",
                    "source": 0,
                    "route_id": 7,
                    "execution_id": 0,
                    "burst_index": 1,
                    "words": 4,
                    "schedule_tick": 310,
                    "dma_start_cycle": 20,
                    "dma_completion_cycle": 24,
                    "event_tick": 310,
                    "service_cycles": 4,
                },
                {
                    "tile_id": 1,
                    "event": "complete",
                    "source": 0,
                    "route_id": 7,
                    "execution_id": 0,
                    "burst_index": 1,
                    "words": 4,
                    "schedule_tick": 310,
                    "dma_start_cycle": 20,
                    "dma_completion_cycle": 24,
                    "event_tick": 350,
                    "service_cycles": 4,
                },
            ],
        )

        write_csv(
            profile / "tile-1-waits.csv",
            [
                "tile_id",
                "event_sequence",
                "reason",
                "start_tick",
                "finish_tick",
                "duration_ticks",
            ],
            [
                {
                    "tile_id": 1,
                    "event_sequence": 4,
                    "reason": "nic-receive-wait",
                    "start_tick": 200,
                    "finish_tick": 350,
                    "duration_ticks": 150,
                }
            ],
        )

        write_csv(
            profile / "tile-0-analog.csv",
            [
                "tile_id",
                "ticket",
                "operation",
                "array_id",
                "phase",
                "device_cycle",
                "event_tick",
            ],
            [
                {
                    "tile_id": 0,
                    "ticket": 2,
                    "operation": 3,
                    "array_id": 0,
                    "phase": "submitted",
                    "device_cycle": 0,
                    "event_tick": 100,
                },
                {
                    "tile_id": 0,
                    "ticket": 2,
                    "operation": 3,
                    "array_id": 0,
                    "phase": "compute-start",
                    "device_cycle": 1,
                    "event_tick": 110,
                },
                {
                    "tile_id": 0,
                    "ticket": 2,
                    "operation": 3,
                    "array_id": 0,
                    "phase": "compute-finish",
                    "device_cycle": 9,
                    "event_tick": 118,
                },
                {
                    "tile_id": 0,
                    "ticket": 2,
                    "operation": 3,
                    "array_id": 0,
                    "phase": "complete",
                    "device_cycle": 9,
                    "event_tick": 118,
                },
            ],
        )

        write_csv(
            profile / "tile-0-memory.csv",
            [
                "tile_id",
                "event",
                "request_id",
                "address",
                "size",
                "direction",
                "issue_tick",
                "event_tick",
                "latency_ticks",
            ],
            [
                {
                    "tile_id": 0,
                    "event": "issue",
                    "request_id": 3,
                    "address": 4096,
                    "size": 8,
                    "direction": "read",
                    "issue_tick": 50,
                    "event_tick": 50,
                    "latency_ticks": 0,
                },
                {
                    "tile_id": 0,
                    "event": "response",
                    "request_id": 3,
                    "address": 4096,
                    "size": 8,
                    "direction": "read",
                    "issue_tick": 50,
                    "event_tick": 75,
                    "latency_ticks": 25,
                },
            ],
        )

        task_fields = [
            "sim_time_ticks",
            "event",
            "tile_id",
            "task_id",
            "execution_id",
        ]
        write_csv(
            profile / "tile-0.csv",
            task_fields,
            [
                {
                    "sim_time_ticks": 100,
                    "event": "start",
                    "tile_id": 0,
                    "task_id": 10,
                    "execution_id": 0,
                },
                {
                    "sim_time_ticks": 200,
                    "event": "finish",
                    "tile_id": 0,
                    "task_id": 10,
                    "execution_id": 0,
                },
            ],
        )
        write_csv(
            profile / "tile-1.csv",
            task_fields,
            [
                {
                    "sim_time_ticks": 400,
                    "event": "start",
                    "tile_id": 1,
                    "task_id": 20,
                    "execution_id": 0,
                },
                {
                    "sim_time_ticks": 800,
                    "event": "finish",
                    "tile_id": 1,
                    "task_id": 20,
                    "execution_id": 0,
                },
            ],
        )

        partitioned_mlir = root / "partitioned.mlir"
        partitioned_mlir.write_text(
            """
builtin.module attributes {
  sculptor.schedule.mesh_cols = 2 : i64,
  sculptor.deployment.routes = [
    #sculptor.deployment_route<
      id = 7 : i64,
      sourceCore = 0 : i64,
      sourceTask = 10 : i64,
      sourceOutput = 0 : i64,
      destinationCore = 1 : i64,
      destinationTask = 20 : i64,
      destinationInput = 0 : i64,
      resourceId = 9 : i64,
      byteSize = 16 : i64
    >
  ]
}
""",
            encoding="utf-8",
        )
        route_manifest = root / "routes.csv"
        subprocess.run(
            [
                "python3",
                str(ROUTE_EXTRACTOR),
                str(partitioned_mlir),
                str(route_manifest),
            ],
            check=True,
        )
        with route_manifest.open(
            "r", encoding="utf-8", newline=""
        ) as source:
            extracted_route = next(csv.DictReader(source))
        assert extracted_route["payload_words"] == "4"
        assert extracted_route["manhattan_hops"] == "1"

        router_statistics = root / "router-statistics.csv"
        write_csv(
            router_statistics,
            [
                "ComponentName",
                "StatisticName",
                "StatisticSubId",
                "Sum.u64",
            ],
            [
                {
                    "ComponentName": "router_0",
                    "StatisticName": "send_bit_count",
                    "StatisticSubId": "port1",
                    "Sum.u64": 288,
                },
                {
                    "ComponentName": "router_0",
                    "StatisticName": "output_port_stalls",
                    "StatisticSubId": "port1",
                    "Sum.u64": 3000,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "flits_forwarded",
                    "StatisticSubId": "west",
                    "Sum.u64": 10,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "packets_forwarded",
                    "StatisticSubId": "west",
                    "Sum.u64": 2,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "output_credit_stall_cycles",
                    "StatisticSubId": "west",
                    "Sum.u64": 7,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "switch_arbitration_stall_cycles",
                    "StatisticSubId": "west",
                    "Sum.u64": 11,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "input_buffer_full_cycles",
                    "StatisticSubId": "west",
                    "Sum.u64": 5,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "output_link_busy_cycles",
                    "StatisticSubId": "west",
                    "Sum.u64": 10,
                },
                {
                    "ComponentName": "router_1_0",
                    "StatisticName": "flits_forwarded",
                    "StatisticSubId": "local",
                    "Sum.u64": 1000,
                },
            ],
        )

        subprocess.run(
            [
                "python3",
                str(ANALYZER),
                str(profile),
                str(report),
                "--router-statistics",
                str(router_statistics),
                "--task-trace-directory",
                str(profile),
                "--route-manifest",
                str(route_manifest),
                "--mesh-link-clock",
                "2GHz",
            ],
            check=True,
        )

        summary = json.loads(
            (report / "summary.json").read_text(encoding="utf-8")
        )
        assert summary["network"]["packets"] == 2
        assert summary["network"]["injected_words"] == 9
        assert summary["network"]["payload_words"] == 4
        assert summary["network"]["directional_word_hops"] == 9
        assert summary["network"]["physical_router_link_bits"] == 608
        assert summary["network"]["router_stalls"] == 24
        assert summary["network"]["output_credit_stall_cycles"] == 13
        assert summary["network"][
            "switch_arbitration_stall_cycles"
        ] == 11
        assert summary["network"]["input_buffer_full_cycles"] == 5
        assert summary["receive_dma"]["transfers"] == 1
        assert summary["analog"]["operations"] == 1
        assert summary["memory"]["modeled_requests"] == 1
        assert summary["transmit_backpressure"] == {
            "blocked_ticks": 15,
            "detail_available": True,
            "events": 1,
            "maximum_queue_occupancy": 5,
            "retries": 3,
        }
        assert summary["tasks"]["critical_chain_tasks"] == 2

        with (report / "memory-caller-sites.csv").open(
            "r", encoding="utf-8", newline=""
        ) as source:
            memory_caller = next(csv.DictReader(source))
        assert memory_caller["invocations"] == "1"
        assert memory_caller["requests"] == "1"
        assert memory_caller["bytes"] == "8"
        assert memory_caller["memory_stall_ticks"] == "25"

        with (report / "routes.csv").open(
            "r", encoding="utf-8", newline=""
        ) as source:
            route = next(csv.DictReader(source))
        assert route["route_id"] == "7"
        assert route["payload_matches_manifest"] == "True"
        assert route["ready_tick"] == "350"

        with (report / "critical-path.csv").open(
            "r", encoding="utf-8", newline=""
        ) as source:
            critical = list(csv.DictReader(source))
        assert [row["task_id"] for row in critical] == ["10", "20"]
        assert critical[1]["predecessor_type"] == "route"
        assert critical[1]["route_id"] == "7"

        summary_profile = root / "summary-raw"
        summary_report = root / "summary-report"
        summary_profile.mkdir()
        write_summary(
            summary_profile / "tile-0-summary.csv",
            0,
            800,
            2,
            9,
            9,
        )
        write_summary(
            summary_profile / "tile-1-summary.csv",
            1,
            900,
            0,
            0,
            0,
        )
        subprocess.run(
            [
                "python3",
                str(ANALYZER),
                str(summary_profile),
                str(summary_report),
            ],
            check=True,
        )
        summary_only = json.loads(
            (summary_report / "summary.json").read_text(
                encoding="utf-8"
            )
        )
        assert summary_only["network"]["packets"] == 2
        assert summary_only["network"]["injected_words"] == 9
        assert summary_only["network"][
            "directional_word_hops"
        ] == 9
        assert not summary_only["network"]["packet_detail_available"]
        assert summary_only["wait_ticks_by_reason"][
            "nic-receive-wait"
        ] == 25

        sentinel_profile = root / "sentinel-raw"
        sentinel_report = root / "sentinel-report"
        sentinel_profile.mkdir()
        write_summary(
            sentinel_profile / "tile-0-summary.csv",
            0,
            (1 << 64) - 1,
            0,
            0,
            0,
        )
        write_csv(
            sentinel_profile / "tile-0-waits.csv",
            [
                "tile_id",
                "event_sequence",
                "reason",
                "start_tick",
                "finish_tick",
                "duration_ticks",
            ],
            [
                {
                    "tile_id": 0,
                    "event_sequence": 1,
                    "reason": "memory-access",
                    "start_tick": 100,
                    "finish_tick": 200,
                    "duration_ticks": 100,
                },
                {
                    "tile_id": 0,
                    "event_sequence": 2,
                    "reason": "nic-receive-wait",
                    "start_tick": 200,
                    "finish_tick": (1 << 64) - 1,
                    "duration_ticks": (1 << 64) - 201,
                },
            ],
        )
        subprocess.run(
            [
                "python3",
                str(ANALYZER),
                str(sentinel_profile),
                str(sentinel_report),
            ],
            check=True,
        )
        sentinel_summary = json.loads(
            (sentinel_report / "summary.json").read_text(
                encoding="utf-8"
            )
        )
        assert sentinel_summary["finish_tick"] == 200
        assert sentinel_summary["finish_tick_reconstructed"]
        assert sentinel_summary["wait_ticks_by_reason"] == {
            "memory-access": 100
        }


if __name__ == "__main__":
    main()
