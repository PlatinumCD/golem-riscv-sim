#!/usr/bin/env python3
"""Build a compact, complete Phase-0 execution-residency A/B fixture."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
from typing import Any


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha256(path: Path) -> str:
    return sha256(path.read_bytes())


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def load_validator(project_root: Path) -> Any:
    path = project_root / "tools" / "compiler" / "validate-sculptor-execution-residency.py"
    spec = importlib.util.spec_from_file_location("execution_residency_validator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_audit(
    module: Any, mode: str, source: bytes, output: bytes, phase: int = 0
) -> dict[str, Any]:
    description = (
        f"mode={mode}\n"
        "maximum_members=8\n"
        "maximum_wave_width=2\n"
        "require_positive_benefit=true\n"
    ).encode()
    payload: dict[str, Any] = {
        "schema": module.SCHEMA,
        "schema_version": module.SCHEMA_VERSION,
        "status": "PASS",
        "phase": phase,
        "plan_version": module.PLAN_VERSION,
        "mode": mode,
        "configuration": {
            "maximum_members": 8,
            "maximum_wave_width": 2,
            "require_positive_benefit": True,
        },
        "configuration_sha256": sha256(description),
        "graph_fingerprint": sha256(output),
        "source_ir_sha256": sha256(source),
        "canonical_ir_bytes": len(output),
        "source_ir_bytes": len(source),
        "semantic_ir_changed": False,
        "accounting_complete": False,
    }
    if phase == 0:
        payload.update({field: 0 for field in module.ZERO_CHANGE_FIELDS})
        payload.update({field: {} for field in module.EMPTY_OBJECT_FIELDS})
        payload.update({field: [] for field in module.EMPTY_ARRAY_FIELDS})
        payload["candidate_growth_stops_by_reason"] = {}
        payload["recommended_candidate_count"] = 0
        payload["candidate_analog_digital_count"] = 0
    else:
        payload.update({field: 0 for field in module.PHYSICAL_ZERO_FIELDS})
        benefit_fields = (
            "avoided_ram_setup_cycles",
            "avoided_ram_transfer_cycles",
            "predicted_avoided_ram_queue_cycles",
            "avoided_spm_endpoint_cycles",
            "avoided_noc_cycles",
            "avoided_runtime_dispatch_cycles",
            "avoided_dependency_transition_cycles",
            "exposed_analog_digital_overlap_cycles",
        )
        cost_fields = (
            "added_digital_lane_contention",
            "added_analog_lane_contention",
            "lost_inter_tile_parallelism",
            "added_local_assembly_or_reduction",
            "added_spm_bank_or_port_contention",
            "code_and_dispatch_cost",
            "pipeline_fill_and_drain",
        )
        candidate = {
            "candidate_ordinal": 0,
            "anchor_work_unit_id": 1,
            "anchor_operation_id": 1,
            "iteration_begin": 0,
            "iteration_end": 1,
            "iteration_step": 1,
            "member_count": 2,
            "internal_edge_count": 1,
            "estimated_spm_bytes_per_slot": 4096,
            "estimated_avoided_input_bytes": 4096,
            "estimated_avoided_output_bytes": 4096,
            "estimated_avoided_requests": 4,
            "logical_internal_bytes": 4096,
            "saved_physical_bytes": 8192,
            "saved_critical_cycle_lower_bound": 0,
            "members": [
                {
                    "stage_ordinal": 0,
                    "work_unit_id": 1,
                    "operation_id": 1,
                    "role": "analog_mvm",
                },
                {
                    "stage_ordinal": 1,
                    "work_unit_id": 2,
                    "operation_id": 2,
                    "role": "digital_post",
                },
            ],
            "internal_edge_ordinals": [0],
            "rejections": [],
            "growth_stops": ["unrelated_semantic_layer"],
            "recommended": True,
            "disposition": "recommended",
            "mvm_partition_kind": "output_row_independent",
            "cycle_model": {
                "status": "deferred_until_mapping_hardware_profile",
                "complete": False,
                "benefit_cycle_lower_bounds": {
                    field: 0 for field in benefit_fields
                },
                "cost_cycle_lower_bounds": {field: 0 for field in cost_fields},
            },
        }
        payload.update(
            {
                "candidate_count": 1,
                "recommended_candidate_count": 1,
                "candidate_analog_digital_count": 1,
                "candidate_logical_bytes": 4096,
                "estimated_avoided_input_bytes": 4096,
                "estimated_avoided_output_bytes": 4096,
                "estimated_avoided_requests": 4,
                "candidate_rejections_by_reason": {},
                "candidate_growth_stops_by_reason": {
                    "unrelated_semantic_layer": 1
                },
                "per_tile_peak_spm_bytes": {},
                "per_tile_headroom_bytes": {},
                "wave_width_histogram": {},
                "regions": [candidate],
                "tiles": [],
            }
        )
    return payload


def make_compile_run(
    module: Any,
    run: Path,
    mode: str,
    phase: int = 0,
    mode_metadata: bool = False,
) -> None:
    run.mkdir(parents=True)
    (run / "cores").mkdir()
    (run / "deployment").mkdir()
    (run / "memory-reports").mkdir()
    active_tiles = list(range(16))
    source = b"module {}\n"
    residency_output = source
    if mode_metadata:
        residency_output = (
            "module { func.func @forward() attributes "
            f'{{sculptor.execution_residency.mode = "{mode}"}} }}\n'
        ).encode()
    ra_output = residency_output
    if mode_metadata:
        ra_output = residency_output.rstrip() + (
            f' executionResidencyMode = "{mode}"\n'
        ).encode()
    for name in module.DEPLOYMENT_ARTIFACTS:
        if name.endswith(".csv"):
            payload = b"tile,placement\n0,0\n"
        elif name == "04-residency-regions.mlir":
            payload = residency_output
        elif name in ("05-ra-tree.mlir", "06-mapping-plan.mlir", "08-placed.mlir"):
            payload = ra_output
        else:
            payload = source
        (run / "deployment" / name).write_bytes(payload)

    audit = make_audit(module, mode, source, residency_output, phase)
    write_json(run / "execution-residency-audit.json", audit)
    deployment = {
        "schema": "sculptor.deployment",
        "version": 2,
        "active_tile_ids": active_tiles,
        "synchronization": {"mode": "exact_dependencies"},
    }
    write_json(run / "deployment-manifest.json", deployment)
    architecture = {
        "schema": "sculptor.streaming-architecture",
        "schema_version": 1,
        "source_mlir": str((run / "deployment" / "01-canonical.mlir").resolve()),
        "architecture": {
            "fixed_shard_bytes": 4096,
            "global_ram_bytes": 32 * 1024 * 1024 * 1024,
            "max_in_flight": 2,
            "noc_word_bytes": 4,
            "scratchpad_bytes": 2 * 1024 * 1024,
            "version": 1,
        },
    }
    write_json(run / "streaming-architecture.json", architecture)
    materialization = {
        "schema": "sculptor.materialization-audit",
        "audit_schema_version": 1,
        "version": 1,
        "status": "PASS",
        "active_tile_ids": active_tiles,
        "maximum_frame_bytes": 4096,
        "sources": [{"path": str((run / "deployment" / "08-placed.mlir").resolve())}],
        "tiles": {
            str(tile): {"source": str((run / "cores" / f"core-{tile}-finalized.mlir").resolve())}
            for tile in active_tiles
        },
        "counters": {
            "epoch_count": 4,
            "maximum_live_global_ram_bytes": 8192,
            "materialized_input_physical_request_count": 2,
            "materialized_input_physical_byte_count": 1024,
            "materialized_output_physical_request_count": 3,
            "materialized_output_physical_byte_count": 3072,
        },
    }
    write_json(run / "materialization-audit.json", materialization)
    memory = {
        "schema_version": 1,
        "active_tile_count": len(active_tiles),
        "scratchpad_capacity_bytes": 2 * 1024 * 1024,
        "capacity_gate": {"status": "PASS", "errors": []},
        "summaries": {
            "capacity": {"maximums": {"requiredLocalBytes": 4096}}
        },
        "tiles": {
            str(tile): {"artifact": str((run / "cores" / f"core-{tile}.o").resolve())}
            for tile in active_tiles
        },
    }
    write_json(run / "memory-reports" / "tile-memory-summary.json", memory)

    (run / "model.mlir").write_bytes(source)
    (run / "model.expected.json").write_text("{}\n")
    (run / "active-cores.txt").write_text("\n".join(map(str, active_tiles)) + "\n")
    (run / "abi-preflight-summary.txt").write_text(
        "active_tiles=16\npasses=16\nepoch_count=4\nerrors=0\n"
    )
    (run / "idle.elf").write_bytes(b"ELF-idle")
    finalized = (
        "module {\n"
        "  func.func private @routine() attributes "
        "{sculptor.deployment.global_routine_id = 0 : i64}\n"
        "  sculptor.task.create\n"
        "}\n"
    ).encode()
    for tile in active_tiles:
        for suffix in module.CORE_ARTIFACT_SUFFIXES:
            payload = finalized if suffix.endswith(".mlir") else b"object\n"
            (run / "cores" / f"core-{tile}{suffix}").write_bytes(payload)
        (run / f"tile-{tile}.elf").write_bytes(b"ELF-tile\n")

    audit_sha = file_sha256(run / "execution-residency-audit.json")
    checks = {
        name: {"status": "PASS"}
        for name in (
            "abi_preflight",
            "complete_lowering",
            "elf_generation",
            "materialization_audit",
            "memory_validation",
            "optimization_contracts",
        )
    }
    checks["execution_residency_audit"] = {
        "status": "PASS",
        "mode": mode,
        "phase": phase,
        "audit_sha256": audit_sha,
    }
    write_json(
        run / "compile-qualification.json",
        {
            "schema": "sculptor.compile-qualification",
            "status": "PASS",
            "checks": checks,
        },
    )
    audit_bytes = (run / "execution-residency-audit.json").stat().st_size
    write_json(
        run / "run-manifest.json",
        {
            "schema": "sculptor.run-manifest",
            "artifacts": {
                "execution_residency_audit": {
                    "kind": "file",
                    "bytes": audit_bytes,
                    "sha256": audit_sha,
                }
            },
        },
    )


def make_evidence(directory: Path, compile_run: Path) -> None:
    directory.mkdir(parents=True)
    launch = {
        "schema": "golem.sculptor-sst-launch",
        "schema_version": 1,
        "model": "fixture",
        "global_ram_channels": 32,
        "sst_threads": 16,
        "sst_partitioner": "sst.simple",
        "synchronization_mode": "exact_dependencies",
        "source_compile_directory": str(compile_run.resolve()),
        "source_run_manifest_sha256": file_sha256(compile_run / "run-manifest.json"),
        "materialization_audit_sha256": file_sha256(
            compile_run / "materialization-audit.json"
        ),
        "physical_global_dma_work": {
            "input_requests": 2,
            "output_requests": 3,
            "total_requests": 5,
        },
        "runtime_tools": {},
    }
    write_json(directory / "launch.json", launch)
    (directory / "partition.txt").write_text(
        "\n".join(f"tile {tile} thread {tile}" for tile in range(16)) + "\n"
    )
    write_json(
        directory / "partition-summary.json",
        {
            "schema": "golem.sculptor-sst-partition",
            "schema_version": 1,
            "status": "PASS",
            "active_tile_count": 16,
            "requested_threads": 16,
            "occupied_active_tile_threads": 16,
            "occupied_component_threads": 16,
            "threads": [
                {"thread": tile, "active_tile_count": 1, "component_count": 2}
                for tile in range(16)
            ],
            "tile_threads": {str(tile): tile for tile in range(16)},
        },
    )
    pending = {
        "pending_completed_receive_frames": 0,
        "pending_global_dma": 0,
        "pending_incoming_frame_assemblies": 0,
        "pending_memory": 0,
        "pending_network_receives": 0,
        "pending_ready_receive_bursts": 0,
        "pending_receive_dma": 0,
        "pending_receive_dma_descriptors": 0,
    }
    totals = {
        "analog_commands_completed": 8,
        "analog_commands_submitted": 8,
        "cpu_cycles": 200,
        "instructions": 100,
        "network_packets": 4,
        "network_words": 16,
        "physical_global_dma_completed": 5,
        "physical_global_dma_submitted": 5,
        "receive_dma_transfers": 1,
        "simulation_tick": 1000,
        "synchronization_events": 16,
        "synchronization_grants": 16,
        "task_finish_events": 16,
    }
    write_json(
        directory / "progress-classification.json",
        {
            "schema": "golem.sculptor-sst-progress-classification",
            "schema_version": 1,
            "state": "passed",
            "evidence_directory": str(directory.resolve()),
            "expected_tiles": 16,
            "initialization_pass_tiles": 16,
            "simulation_pass_tiles": 16,
            "simulation_error_tiles": [],
            "deltas": {"instructions": 1},
            "totals": totals,
            "pending": pending,
            "physical_global_dma_work": {
                "available": True,
                "completed_requests": 5,
                "completion_fraction": 1.0,
                "remaining_requests": 0,
            },
        },
    )
    (directory / "status.csv").write_text(
        "model,status,exit_code,failure_stage,total_wall_seconds\n"
        "fixture,PASS,0,,1.1\n"
    )
    (directory / "result.csv").write_text(
        "model,status,active_tiles,epoch_count,synchronization_mode,"
        "sync_instruction_quantum,global_ram_channels,simulated_time,"
        "simulation_wall_seconds,output_validation\n"
        "fixture,PASS,16,4,exact_dependencies,1000000,32,1.0 ms,1.00,SKIPPED\n"
    )
    (directory / "resource-usage.txt").write_text(
        "sst_wall_seconds=1.00\nsst_max_rss_kib=1024\n"
    )
    header = (
        "ComponentName,StatisticName,StatisticSubId,StatisticType,SimTime,Rank,"
        "Sum.u64,SumSQ.u64,Count.u64,Min.u64,Max.u64\n"
    )
    rows = [
        "global_ram,requests,,Accumulator,1000,0,5,0,1,5,5",
        "global_ram,bytes,,Accumulator,1000,0,4096,0,1,4096,4096",
        "global_ram,readiness_delay_cycles,,Accumulator,1000,0,30,0,5,2,10",
        "global_ram,queue_delay_cycles,,Accumulator,1000,0,20,0,5,1,8",
        "global_ram,service_cycles,,Accumulator,1000,0,10,0,5,2,2",
        "global_ram,execution_teardown_wait_cycles,,Accumulator,1000,0,15,0,16,0,4",
        "global_ram,maximum_queue_occupancy,,Accumulator,1000,0,3,0,1,3,3",
        "global_ram,readiness_blocked_reads,,Accumulator,1000,0,2,0,1,2,2",
        "global_ram,readiness_publications,,Accumulator,1000,0,3,0,1,3,3",
        "global_ram,readiness_releases,,Accumulator,1000,0,2,0,1,2,2",
    ]
    (directory / "router-statistics.csv").write_text(header + "\n".join(rows) + "\n")
    log = "".join(
        f"SCULPTOR_RA_INIT_PASS tile={tile}\nSCULPTOR_RA_SIM_PASS\n"
        for tile in range(16)
    )
    log += (
        "MITTENS_QEMU_CAPTURE_HOST stop_reason=guest-exit(6) count=16 "
        "total_ns=10 max_ns=1\nSimulation is complete, simulated time: 1.0 ms\n"
    )
    (directory / "simulation.log").write_text(log)


def corrupt(root: Path, kind: str) -> None:
    candidate = root / "candidate"
    evidence = root / "analyze-3"
    if kind == "none":
        return
    if kind == "binary":
        with (candidate / "tile-0.elf").open("ab") as output:
            output.write(b"drift")
    elif kind == "launch_hash":
        payload = json.loads((evidence / "launch.json").read_text())
        payload["source_run_manifest_sha256"] = "0" * 64
        write_json(evidence / "launch.json", payload)
    elif kind == "termination":
        path = evidence / "simulation.log"
        path.write_text(path.read_text().replace("SCULPTOR_RA_SIM_PASS\n", "", 1))
    elif kind == "physical":
        path = evidence / "router-statistics.csv"
        path.write_text(path.read_text().replace(",4096,0,1,4096,4096", ",4097,0,1,4097,4097"))
    elif kind == "pending":
        path = evidence / "progress-classification.json"
        payload = json.loads(path.read_text())
        payload["pending"]["pending_memory"] = 1
        write_json(path, payload)
    elif kind == "simulated_time":
        path = evidence / "result.csv"
        path.write_text(path.read_text().replace("1.0 ms,1.00", "1.1 ms,1.00"))
    elif kind == "placement":
        path = candidate / "deployment" / "08-placed.mlir"
        path.write_bytes(path.read_bytes() + b"physical-placement-drift\n")
    else:
        raise ValueError(f"unknown corruption: {kind}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project_root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "corruption",
        choices=(
            "none", "binary", "launch_hash", "termination", "physical",
            "pending", "simulated_time", "placement",
        ),
    )
    parser.add_argument("--candidate-phase", type=int, choices=(0, 2), default=0)
    args = parser.parse_args()
    if args.output.exists():
        shutil.rmtree(args.output)
    module = load_validator(args.project_root)
    mode_metadata = args.candidate_phase == 2
    make_compile_run(
        module, args.output / "control", "off", mode_metadata=mode_metadata
    )
    make_compile_run(
        module,
        args.output / "candidate",
        "analyze",
        phase=args.candidate_phase,
        mode_metadata=mode_metadata,
    )
    if args.candidate_phase == 0:
        for mode, run in (
            ("off", args.output / "control"),
            ("analyze", args.output / "candidate"),
        ):
            for observation in range(1, 4):
                make_evidence(args.output / f"{mode}-{observation}", run)
    corrupt(args.output, args.corruption)


if __name__ == "__main__":
    main()
