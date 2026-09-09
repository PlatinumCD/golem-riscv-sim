#!/usr/bin/env python3

from __future__ import annotations

import csv
import hashlib
import importlib.util
import json
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
ANALYZER_PATH = PROJECT_ROOT / "scripts" / "analyze-residency-feasibility.py"
SPEC = importlib.util.spec_from_file_location("residency_feasibility", ANALYZER_PATH)
ANALYZER = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(ANALYZER)


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def movement_line(tile_id: int, values: dict[str, int]) -> str:
    fields = " ".join(
        f"{name}={values[name]}" for name in ANALYZER.MOVEMENT_FIELDS
    )
    return f"{ANALYZER.MOVEMENT_PREFIX}tile={tile_id} {fields}\n"


def make_fixture(root: Path, *, phase4: bool = False) -> tuple[Path, Path]:
    compile_dir = root / "compile"
    evidence_dir = root / "evidence"
    compile_dir.mkdir(parents=True)
    evidence_dir.mkdir(parents=True)
    active_tiles = [0, 1]
    epoch_count = 3

    write_csv(
        compile_dir / "status.csv",
        ["case", "status", "exit_code", "failure_stage", "total_wall_seconds"],
        [{"case": "resnet32", "status": "COMPILE_PASS", "exit_code": 0,
          "failure_stage": "", "total_wall_seconds": "2.5"}],
    )
    (compile_dir / "active-cores.txt").write_text("0\n1\n", encoding="utf-8")
    write_json(
        compile_dir / "deployment-manifest.json",
        {"schema": "sculptor.deployment", "version": 2,
         "active_tile_ids": active_tiles,
         "synchronization": {
             "mode": "bulk_barrier",
             "semantic_epoch_count": epoch_count,
         }},
    )
    counters = {
        "epoch_count": epoch_count,
        "materialized_tensor_count": 4,
        "materialized_tensor_bytes": 12288,
        "materialized_producer_region_count": 3,
        "materialized_consumer_region_count": 4,
        "materialized_output_dma_descriptor_count": 3,
        "materialized_input_dma_descriptor_count": 4,
        "materialized_main_transfer_count_logical": 2,
        "materialized_tail_transfer_count_logical": 2,
        "retained_local_owner_alias_count": 0,
        "elided_retained_input_descriptor_count": 0,
        "elided_retained_input_logical_bytes": 0,
        "elided_retained_input_logical_transfer_count": 0,
        "elided_retained_output_descriptor_count": 0,
        "elided_retained_output_logical_bytes": 0,
        "elided_retained_output_logical_transfer_count": 0,
        "cross_epoch_direct_route_count": 0,
        "maximum_live_global_ram_bytes": 10000,
        "unowned_materialized_byte_count": 0,
        "multiply_owned_materialized_byte_count": 0,
        "read_before_produced_region_count": 0,
        "unclassified_boundary_count": 0,
    }
    tile_accounting: list[dict[str, int]] | None = None
    if phase4:
        counters.update(
            {
                "retained_local_owner_alias_count": 2,
                "elided_retained_input_descriptor_count": 2,
                "elided_retained_input_logical_bytes": 8192,
                "elided_retained_input_logical_transfer_count": 2,
                "elided_retained_output_descriptor_count": 1,
                "elided_retained_output_logical_bytes": 4096,
                "elided_retained_output_logical_transfer_count": 1,
                "phase4_accounting_tile_count": 2,
                "retained_local_policy_enabled_tile_count": 2,
                "retained_local_logical_transfer_count": 2,
                "retained_local_logical_bytes": 8192,
                "materialized_input_physical_request_count": 3,
                "materialized_output_physical_request_count": 1,
                "materialized_input_physical_byte_count": 8192,
                "materialized_output_physical_byte_count": 4096,
                "retained_input_physical_request_count_before_elision": 5,
                "retained_output_physical_request_count_before_elision": 2,
                "retained_input_physical_byte_count_before_elision": 16384,
                "retained_output_physical_byte_count_before_elision": 8192,
                "elided_retained_input_physical_request_count": 2,
                "elided_retained_output_physical_request_count": 1,
                "elided_retained_input_physical_byte_count": 8192,
                "elided_retained_output_physical_byte_count": 4096,
            }
        )
        tile_accounting = [
            {
                "tile_id": 0,
                "retained_local_policy_enabled_tile_count": 1,
                "retained_local_logical_transfer_count": 1,
                "retained_local_logical_bytes": 4096,
                "materialized_input_physical_request_count": 2,
                "materialized_output_physical_request_count": 0,
                "materialized_input_physical_byte_count": 4096,
                "materialized_output_physical_byte_count": 0,
                "retained_input_physical_request_count_before_elision": 3,
                "retained_output_physical_request_count_before_elision": 0,
                "retained_input_physical_byte_count_before_elision": 8192,
                "retained_output_physical_byte_count_before_elision": 0,
                "elided_retained_input_physical_request_count": 1,
                "elided_retained_output_physical_request_count": 0,
                "elided_retained_input_physical_byte_count": 4096,
                "elided_retained_output_physical_byte_count": 0,
            },
            {
                "tile_id": 1,
                "retained_local_policy_enabled_tile_count": 1,
                "retained_local_logical_transfer_count": 1,
                "retained_local_logical_bytes": 4096,
                "materialized_input_physical_request_count": 1,
                "materialized_output_physical_request_count": 1,
                "materialized_input_physical_byte_count": 4096,
                "materialized_output_physical_byte_count": 4096,
                "retained_input_physical_request_count_before_elision": 2,
                "retained_output_physical_request_count_before_elision": 2,
                "retained_input_physical_byte_count_before_elision": 8192,
                "retained_output_physical_byte_count_before_elision": 8192,
                "elided_retained_input_physical_request_count": 1,
                "elided_retained_output_physical_request_count": 1,
                "elided_retained_input_physical_byte_count": 4096,
                "elided_retained_output_physical_byte_count": 4096,
            },
        ]
    materialization_audit = {
        "schema": "sculptor.materialization-audit",
        "audit_schema_version": 1,
        "status": "PASS",
        "errors": [],
        "active_tile_ids": active_tiles,
        "counters": counters,
    }
    if tile_accounting is not None:
        materialization_audit["tiles"] = tile_accounting
    write_json(
        compile_dir / "materialization-audit.json",
        materialization_audit,
    )
    write_json(
        compile_dir / "memory-reports" / "tile-memory-summary.json",
        {
            "schema_version": 1,
            "active_tile_count": 2,
            "summaries": {
                "audit": {"maximums": {
                    "escaping_allocation_count": 0,
                    "missing_deallocation_count": 0,
                    "unplanned_allocation_count": 0,
                    "unplanned_copy_count": 0,
                    "unplanned_full_tensor_copy_count": 0,
                }},
                "capacity": {"maximums": {
                    "requiredLocalBytes": 4096,
                    "scratchpadBytes": 4096,
                    "persistentBytes": 0,
                }},
                "finalized": {"maximums": {
                    "route_input_bytes": 0,
                    "route_output_bytes": 0,
                }},
                "physical": {"maximums": {
                    "static_image_bytes": 1024,
                    "estimated_heap_headroom_bytes": 65536,
                }},
            },
            "tiles": {
                "0": {"physical": {"estimated_heap_headroom_bytes": 65000}},
                "1": {"physical": {"estimated_heap_headroom_bytes": 64000}},
            },
        },
    )
    manifest = {
        "schema": "golem.sculptor-run",
        "schema_version": 1,
        "run": {"mode": "compile", "model": "resnet32", "id": "fixture"},
        "hardware": {
            "array_columns": 512,
            "array_rows": 1024,
            "arrays_per_core": 4,
            "mesh_columns": 2,
            "mesh_rows": 1,
        },
        "environment": {
            "GOLEM_MODEL_CPU_CLOCK": "1GHz",
            "GOLEM_MODEL_CPU_ISSUE_WIDTH": "1",
            "GOLEM_MODEL_DIGITAL_WORKERS": "1",
        },
        "architecture_manifest": {"architecture": {
            "fixed_shard_bytes": 4096,
            "scratchpad_bytes": 8192,
            "global_ram_bytes": 1048576,
        }},
    }
    write_json(compile_dir / "run-manifest.json", manifest)

    write_csv(
        evidence_dir / "status.csv",
        ["model", "status", "exit_code", "failure_stage", "total_wall_seconds"],
        [{"model": "resnet32", "status": "PASS", "exit_code": 0,
          "failure_stage": "", "total_wall_seconds": "1.5"}],
    )
    write_csv(
        evidence_dir / "result.csv",
        ["model", "status", "active_tiles", "epoch_count", "global_ram_channels",
         "simulated_time", "output_validation"],
        [{"model": "resnet32", "status": "PASS", "active_tiles": 2,
          "epoch_count": 3, "global_ram_channels": 4,
          "simulated_time": "0.0005 ms", "output_validation": "SKIPPED"}],
    )
    write_json(
        evidence_dir / "launch.json",
        {
            "schema": "golem.sculptor-sst-reuse",
            "schema_version": 1,
            "model": "resnet32",
            "source_compile_directory": str(compile_dir),
            "source_run_manifest_sha256": sha256(compile_dir / "run-manifest.json"),
            "materialization_audit_sha256": sha256(compile_dir / "materialization-audit.json"),
            "hardware": manifest["hardware"],
            "global_ram_channels": 4,
        },
    )
    (evidence_dir / "simulation.log").write_text(
        "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=0 released_epoch=1 "
        "arrivals=2 idle=0 first_arrival_cycle=10 release_cycle=100\n"
        "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=1 released_epoch=2 "
        "arrivals=2 idle=1 first_arrival_cycle=120 release_cycle=300\n"
        "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=2 released_epoch=3 "
        "arrivals=2 idle=1 first_arrival_cycle=320 release_cycle=450\n"
        "Simulation is complete, simulated time: 0.0005 ms\n"
        "SCULPTOR_RA_SIM_PASS\nSCULPTOR_RA_SIM_PASS\n",
        encoding="utf-8",
    )
    for tile_id in active_tiles:
        path = evidence_dir / "uart" / f"tile-{tile_id}.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        uart = f"SCULPTOR_RA_INIT_PASS tile={tile_id}\n"
        if phase4:
            movement = (
                {
                    "physical_global_ram_dma_requests": 2,
                    "physical_global_ram_dma_completions": 2,
                    "physical_global_ram_dma_bytes": 4096,
                    "physical_noc_frames_sent": 1,
                    "physical_noc_frames_received": 0,
                    "physical_noc_payload_bytes_sent": 4096,
                    "physical_noc_payload_bytes_received": 0,
                    "local_copy_transfers": 0,
                    "local_copy_bytes": 0,
                    "retained_forwarded_logical_transfers": 1,
                    "retained_forwarded_logical_bytes": 4096,
                }
                if tile_id == 0
                else {
                    "physical_global_ram_dma_requests": 2,
                    "physical_global_ram_dma_completions": 2,
                    "physical_global_ram_dma_bytes": 8192,
                    "physical_noc_frames_sent": 0,
                    "physical_noc_frames_received": 1,
                    "physical_noc_payload_bytes_sent": 0,
                    "physical_noc_payload_bytes_received": 4096,
                    "local_copy_transfers": 0,
                    "local_copy_bytes": 0,
                    "retained_forwarded_logical_transfers": 1,
                    "retained_forwarded_logical_bytes": 4096,
                }
            )
            uart += movement_line(tile_id, movement)
        uart += "SCULPTOR_RA_SIM_PASS\n"
        path.write_text(uart, encoding="utf-8")
        values = {
            "tile_id": tile_id,
            "instructions": 100 * (tile_id + 1),
            "cpu_cycles": 120 * (tile_id + 1),
            "vector_instructions": 10 * (tile_id + 1),
            "synchronization_events": 4,
            "analog_active_cycles": 20 * (tile_id + 1),
            "analog_link_beats": 8,
            "network_packets": 0,
            "network_words": 0,
            "network_word_hops": 0,
            "network_transit_ticks": 0,
            "network_endpoint_queue_ticks": 0,
            "receive_dma_active_cycles": 0,
            "wait_epoch-barrier-arrive_ticks": 10,
            "wait_scratchpad-dma-submit_ticks": 0,
            "wait_scratchpad-dma-wait_ticks": 5,
        }
        write_csv(
            evidence_dir / "trace" / "performance" / f"tile-{tile_id}-summary.csv",
            ["metric", "value"],
            [{"metric": key, "value": value} for key, value in values.items()],
        )
    router_rows = [
        ("global_ram", "requests", "", 4),
        ("global_ram", "bytes", "", 12288),
        ("global_ram", "readiness_delay_cycles", "", 12),
        ("global_ram", "queue_delay_cycles", "", 4),
        ("global_ram", "service_cycles", "", 96),
        ("global_ram", "execution_teardown_wait_cycles", "", 8),
        ("global_ram", "maximum_queue_occupancy", "", 2),
        ("epoch_barrier", "arrivals", "", 6),
        ("epoch_barrier", "idle_arrivals", "", 2),
        ("epoch_barrier", "releases", "", 3),
        ("epoch_barrier", "barrier_wait_cycles", "", 100),
        ("router_0_0", "flits_forwarded", "east", 7),
        ("router_0_0", "packets_forwarded", "east", 2),
        ("router_0_0", "output_credit_stall_cycles", "east", 1),
        ("router_0_0", "flits_forwarded", "local", 1000),
    ]
    write_csv(
        evidence_dir / "router-statistics.csv",
        ["ComponentName", "StatisticName", "StatisticSubId", "StatisticType",
         "SimTime", "Rank", "Sum.u64", "SumSQ.u64", "Count.u64", "Min.u64", "Max.u64"],
        [{"ComponentName": component, "StatisticName": metric,
          "StatisticSubId": port, "StatisticType": "Accumulator",
          "SimTime": 500000, "Rank": 0, "Sum.u64": value,
          "SumSQ.u64": value * value, "Count.u64": 1,
          "Min.u64": value, "Max.u64": value}
         for component, metric, port, value in router_rows],
    )
    return compile_dir, evidence_dir


def expect_failure(compile_dir: Path, evidence_dir: Path, fragment: str) -> None:
    try:
        ANALYZER.analyze(compile_dir, evidence_dir)
    except ANALYZER.AuditError as error:
        assert fragment in str(error), (fragment, str(error))
    else:
        raise AssertionError(f"expected AuditError containing {fragment!r}")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    assert text.count(old) == 1, (path, old, text.count(old))
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="residency-feasibility-") as directory:
        compile_dir, evidence_dir = make_fixture(Path(directory))
        payload = ANALYZER.analyze(compile_dir, evidence_dir)
        assert payload["schema"] == "golem.residency-feasibility"
        assert payload["status"] == "PASS"
        assert payload["validation"]["model_outputs_checked"] is False
        assert payload["completion"]["active_tiles_completed"] == 2
        assert payload["completion"]["epoch_releases_completed"] == 3
        assert payload["epoch_parallelism"]["working_tile_epochs"] == 4
        assert payload["epoch_parallelism"]["tile_epoch_utilization"] == 4 / 6
        assert payload["epoch_parallelism"]["working_tile_histogram"] == {"1": 2, "2": 1}
        assert payload["component_lower_bounds"]["cpu_instruction_issue"]["cycles"] == 150
        assert payload["component_lower_bounds"]["global_ram_channel_service"]["cycles"] == 24
        assert payload["fabric"]["noc_nonlocal"]["flits_forwarded"] == 7
        assert payload["scratchpad_feasibility"]["maximum_live_global_fits_aggregate_spm"]
        assert payload["scratchpad_feasibility"]["minimum_estimated_heap_headroom_bytes"] == 64000

        # The current aggregate-audit writer includes a complete, all-zero
        # Phase-4 counter block for a legacy compile and marks that mode with a
        # zero accounting-tile count.  Such immutable evidence remains legacy
        # and therefore does not require movement summaries.
        legacy_audit_path = compile_dir / "materialization-audit.json"
        legacy_audit = json.loads(legacy_audit_path.read_text(encoding="utf-8"))
        for name in ANALYZER.PHASE4_MOVEMENT_AUDIT_FIELDS:
            legacy_audit["counters"].setdefault(name, 0)
        write_json(legacy_audit_path, legacy_audit)
        legacy_launch_path = evidence_dir / "launch.json"
        legacy_launch = json.loads(legacy_launch_path.read_text(encoding="utf-8"))
        legacy_launch["materialization_audit_sha256"] = sha256(legacy_audit_path)
        write_json(legacy_launch_path, legacy_launch)
        legacy_payload = ANALYZER.analyze(compile_dir, evidence_dir)
        assert legacy_payload["phase4_materialization"] is None
        assert legacy_payload["fabric"]["movement"] is None

        # Removing one half of a compiler-proven compact pair may reduce its
        # payload while leaving one physical request both before and after.
        # This is a valid differential certificate: bytes saved is positive,
        # request-count delta is zero, and both absolute streams remain within
        # the configured physical-frame envelope.
        compact_half_tile = {
            "tile_id": 0,
            "retained_local_policy_enabled_tile_count": 1,
            "retained_local_logical_transfer_count": 1,
            "retained_local_logical_bytes": 2048,
            "materialized_input_physical_request_count": 1,
            "materialized_output_physical_request_count": 0,
            "materialized_input_physical_byte_count": 2048,
            "materialized_output_physical_byte_count": 0,
            "retained_input_physical_request_count_before_elision": 1,
            "retained_output_physical_request_count_before_elision": 0,
            "retained_input_physical_byte_count_before_elision": 4096,
            "retained_output_physical_byte_count_before_elision": 0,
            "elided_retained_input_physical_request_count": 0,
            "elided_retained_output_physical_request_count": 0,
            "elided_retained_input_physical_byte_count": 2048,
            "elided_retained_output_physical_byte_count": 0,
        }
        compact_half_counters = {
            "phase4_accounting_tile_count": 1,
            **{
                name: value
                for name, value in compact_half_tile.items()
                if name != "tile_id"
            },
        }
        compact_half_phase4 = ANALYZER._phase4_materialization(
            {
                "counters": compact_half_counters,
                "tiles": [compact_half_tile],
            },
            [0],
            4096,
        )
        assert compact_half_phase4 is not None
        assert compact_half_phase4["directions"]["input"][
            "elided_requests"
        ] == 0
        assert compact_half_phase4["directions"]["input"][
            "elided_bytes"
        ] == 2048

        phase4_compile, phase4_evidence = make_fixture(
            Path(directory) / "phase4", phase4=True
        )
        phase4_payload = ANALYZER.analyze(phase4_compile, phase4_evidence)
        movement = phase4_payload["fabric"]["movement"]
        assert movement["tile_count"] == 2
        assert movement["totals"] == {
            "physical_global_ram_dma_requests": 4,
            "physical_global_ram_dma_completions": 4,
            "physical_global_ram_dma_bytes": 12288,
            "physical_noc_frames_sent": 1,
            "physical_noc_frames_received": 1,
            "physical_noc_payload_bytes_sent": 4096,
            "physical_noc_payload_bytes_received": 4096,
            "local_copy_transfers": 0,
            "local_copy_bytes": 0,
            "retained_forwarded_logical_transfers": 2,
            "retained_forwarded_logical_bytes": 8192,
        }
        assert movement["phase4_certificate"] == {
            "status": "PASS",
            "compiler": {
                "before_requests": 7,
                "before_bytes": 24576,
                "remaining_requests": 4,
                "remaining_bytes": 12288,
                "elided_requests": 3,
                "elided_bytes": 12288,
            },
            "compiler_directions": {
                "input": {
                    "before_requests": 5,
                    "before_bytes": 16384,
                    "remaining_requests": 3,
                    "remaining_bytes": 8192,
                    "elided_requests": 2,
                    "elided_bytes": 8192,
                },
                "output": {
                    "before_requests": 2,
                    "before_bytes": 8192,
                    "remaining_requests": 1,
                    "remaining_bytes": 4096,
                    "elided_requests": 1,
                    "elided_bytes": 4096,
                },
            },
            "runtime_remaining_requests": 4,
            "runtime_remaining_bytes": 12288,
            "retained_forwarded_logical_transfers": 2,
            "retained_forwarded_logical_bytes": 8192,
        }
        assert phase4_payload["validation"]["model_outputs_checked"] is False
        assert "phase4_zero_copy_retained_forwarding" in phase4_payload["validation"]["checks"]

        phase4_uart0 = phase4_evidence / "uart" / "tile-0.log"
        original_uart0 = phase4_uart0.read_text(encoding="utf-8")
        movement_summary = next(
            line
            for line in original_uart0.splitlines()
            if line.startswith(ANALYZER.MOVEMENT_PREFIX)
        )

        phase4_uart0.write_text(
            original_uart0.replace(movement_summary + "\n", ""),
            encoding="utf-8",
        )
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "movement summaries do not exactly match active tiles",
        )
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        phase4_uart0.write_text(
            original_uart0.replace(
                movement_summary + "\n", movement_summary + "\n" + movement_summary + "\n"
            ),
            encoding="utf-8",
        )
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "exactly one movement summary",
        )
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        replace_once(
            phase4_uart0,
            "physical_global_ram_dma_completions=2",
            "physical_global_ram_dma_completions=1",
        )
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "tile 0 physical global-RAM DMA requests and completions are unbalanced",
        )
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        replace_once(
            phase4_uart0,
            "physical_global_ram_dma_bytes=4096",
            "physical_global_ram_dma_bytes=4095",
        )
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "movement and router physical global-RAM byte counts disagree",
        )
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        replace_once(
            phase4_uart0,
            "physical_noc_frames_sent=1",
            "physical_noc_frames_sent=2",
        )
        expect_failure(phase4_compile, phase4_evidence, "NoC sent and received")
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        replace_once(
            phase4_uart0, "local_copy_bytes=0", "local_copy_bytes=1"
        )
        expect_failure(phase4_compile, phase4_evidence, "local payload copy")
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        replace_once(
            phase4_uart0,
            "retained_forwarded_logical_transfers=1",
            "retained_forwarded_logical_transfers=2",
        )
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "runtime and compiler retained logical transfer counts disagree",
        )
        phase4_uart0.write_text(original_uart0, encoding="utf-8")

        phase4_audit_path = phase4_compile / "materialization-audit.json"
        original_phase4_audit = phase4_audit_path.read_text(encoding="utf-8")
        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["counters"][
            "retained_input_physical_request_count_before_elision"
        ] += 1
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(phase4_compile, phase4_evidence, "are not conserved")
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["counters"][
            "elided_retained_input_physical_request_count"
        ] = 0
        phase4_audit["counters"][
            "retained_input_physical_request_count_before_elision"
        ] = 3
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "configured physical-frame envelope",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["counters"]["retained_local_logical_bytes"] += 1
        phase4_audit["tiles"][0]["retained_local_logical_bytes"] += 1
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "forwarded logical bytes disagree with elided input physical bytes",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        del phase4_audit["counters"]["materialized_input_physical_request_count"]
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "incomplete Phase-4 accounting schema",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["counters"]["phase4_accounting_tile_count"] = 0
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "zero Phase-4 accounting tile count has nonzero counters",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["counters"]["retained_local_policy_enabled_tile_count"] = 0
        for tile in phase4_audit["tiles"]:
            tile["retained_local_policy_enabled_tile_count"] = 0
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "disabled retained-local policy reports Phase-4 retention activity",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        phase4_audit = json.loads(original_phase4_audit)
        phase4_audit["tiles"][0]["retained_local_logical_bytes"] += 1
        write_json(phase4_audit_path, phase4_audit)
        expect_failure(
            phase4_compile,
            phase4_evidence,
            "does not sum to its global value",
        )
        phase4_audit_path.write_text(original_phase4_audit, encoding="utf-8")

        legacy_audit_path = compile_dir / "materialization-audit.json"
        original_legacy_audit = legacy_audit_path.read_text(encoding="utf-8")
        legacy_audit = json.loads(original_legacy_audit)
        for name in ANALYZER.PHASE4_MOVEMENT_AUDIT_FIELDS:
            legacy_audit["counters"].pop(name, None)
        legacy_audit["counters"][
            "elided_retained_input_physical_request_count"
        ] = 0
        write_json(legacy_audit_path, legacy_audit)
        expect_failure(
            compile_dir,
            evidence_dir,
            "incomplete Phase-4 accounting schema",
        )
        legacy_audit_path.write_text(original_legacy_audit, encoding="utf-8")

        result_path = evidence_dir / "result.csv"
        original_result = result_path.read_text(encoding="utf-8")
        replace_once(result_path, "SKIPPED", "CHECKED")
        expect_failure(compile_dir, evidence_dir, "explicitly skip")
        result_path.write_text(original_result, encoding="utf-8")

        simulation_path = evidence_dir / "simulation.log"
        original_simulation = simulation_path.read_text(encoding="utf-8")
        simulation_path.write_text(
            original_simulation.replace(
                "MITTENS_EPOCH_BARRIER_RELEASE completed_epoch=2 released_epoch=3 "
                "arrivals=2 idle=1 first_arrival_cycle=320 release_cycle=450\n",
                "",
            ),
            encoding="utf-8",
        )
        expect_failure(compile_dir, evidence_dir, "epoch releases")
        simulation_path.write_text(original_simulation, encoding="utf-8")

        simulation_path.write_text(
            original_simulation.replace("arrivals=2 idle=1", "arrivals=1 idle=1", 1),
            encoding="utf-8",
        )
        expect_failure(compile_dir, evidence_dir, "arrival count")
        simulation_path.write_text(original_simulation, encoding="utf-8")

        audit_path = compile_dir / "materialization-audit.json"
        audit = json.loads(audit_path.read_text(encoding="utf-8"))
        audit["counters"]["read_before_produced_region_count"] = 1
        write_json(audit_path, audit)
        expect_failure(compile_dir, evidence_dir, "read_before_produced_region_count")
        audit["counters"]["read_before_produced_region_count"] = 0
        write_json(audit_path, audit)
        launch = json.loads((evidence_dir / "launch.json").read_text(encoding="utf-8"))
        launch["materialization_audit_sha256"] = sha256(audit_path)
        write_json(evidence_dir / "launch.json", launch)

        summary = evidence_dir / "trace" / "performance" / "tile-1-summary.csv"
        summary.unlink()
        expect_failure(compile_dir, evidence_dir, "performance summaries")


if __name__ == "__main__":
    main()
