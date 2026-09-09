#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[3]
VALIDATOR = PROJECT_ROOT / "tools" / "compiler" / "validate-sculptor-scalar-region-differential.py"
STAGES = (
    "01-canonical.mlir",
    "02-converted.mlir",
    "03-layouts.mlir",
    "03-golem.mlir",
    "03-duplicate-matrices.mlir",
    "03-resolved-layouts.mlir",
    "04-expanded-digital-work.mlir",
    "04-parametric-work.mlir",
    "04-tensor-fragments.mlir",
    "04-residency-regions.mlir",
    "05-ra-tree.mlir",
    "06-mapping-plan.mlir",
    "08-placed.mlir",
)
CHECKS = (
    "complete_lowering",
    "execution_residency_audit",
    "materialization_audit",
    "abi_preflight",
    "elf_generation",
    "memory_validation",
    "optimization_contracts",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def descriptor(
    descriptor_id: int,
    operation: int,
    work: int,
    tensor: int,
    direction: int,
    boundary: int,
    flags: int = 0,
    *,
    epoch: int = 1,
    port: int = 0,
    loop: int = -1,
    segment_offset: int = 0,
) -> str:
    return (
        "#sculptor.materialized_dma_descriptor<"
        f"id = {descriptor_id} : i64, operationId = {operation} : i64, "
        f"epochId = {epoch} : i64, "
        f"workUnitId = {work} : i64, tensorId = {tensor} : i64, "
        f"portNumber = {port} : i64, loopId = {loop} : i64, "
        f"direction = {direction} : i64, templateKind = 0 : i64, "
        f"flags = {flags} : i64, scratchpadRingBase = 0 : i64, "
        "scratchpadSlotStride = 4 : i64, ringSlots = 1 : i64, "
        "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
        f"iterationStep = 1 : i64, segmentOffset = {segment_offset} : i64, "
        "segmentCount = 1 : i64, bytesPerIteration = 4 : i64, "
        f"boundaryId = {boundary} : i64>"
    )


def core_module(candidate: bool) -> str:
    regenerated = (
        {"epoch": 2, "port": 3, "loop": 7, "segment_offset": 9}
        if candidate
        else {}
    )
    external_input = descriptor(
        0, 10 if not candidate else 20, 10 if not candidate else 20,
        100, 0, (1 << 32) - 1, flags=1, **regenerated
    )
    external_output = descriptor(
        1, 12 if not candidate else 22, 12 if not candidate else 22,
        102, 1, (1 << 32) - 1, flags=2, **regenerated
    )
    internal_input = descriptor(2, 11, 11, 101, 0, 7)
    internal_output = descriptor(3, 10, 10, 101, 1, 7)
    selected_descriptors = (
        [external_input, external_output]
        if candidate
        else [external_input, external_output, internal_input, internal_output]
    )
    scalar = ""
    if candidate:
        scalar = (
            "sculptor.execution_residency.scalar_plan_version = 3 : i64, "
            "sculptor.execution_residency.scalar_contracted_count = 1 : i64, "
            "sculptor.execution_residency.scalar_fallback_count = 0 : i64, "
            "sculptor.execution_residency.scalar_removed_kernel_count = 2 : i64, "
            "sculptor.execution_residency.scalar_regions = ["
            "{"
            "incremental_over_retained_control_input_bytes = 4 : i64, "
            "incremental_over_retained_control_input_descriptor_count = 1 : i64, "
            "incremental_over_retained_control_input_request_count = 1 : i64, "
            "incremental_over_retained_control_output_bytes = 4 : i64, "
            "incremental_over_retained_control_output_descriptor_count = 1 : i64, "
            "incremental_over_retained_control_output_request_count = 1 : i64, "
            "input_port_count = 1 : i64, internal_boundary_ids = [7], "
            "output_port_count = 1 : i64, "
            "pre_contraction_input_bytes = 8 : i64, "
            "pre_contraction_input_descriptor_count = 2 : i64, "
            "pre_contraction_input_request_count = 2 : i64, "
            "pre_contraction_output_bytes = 8 : i64, "
            "pre_contraction_output_descriptor_count = 2 : i64, "
            "pre_contraction_output_request_count = 2 : i64, "
            "region_id = 0 : i64, removed_kernel_count = 2 : i64, "
            "schema_version = 3 : i64, wrapper_routine_id = 0 : i64}], "
        )
    physical_region = (
        "{actual_region_owner_bytes = 4 : i64, actual_ring_slots = 1 : i64, "
        "actual_tile_assembly_bytes = 0 : i64, actual_tile_peak_live_bytes = 8 : i64, "
        "actual_tile_required_local_bytes = 8 : i64, "
        "actual_tile_routine_temporary_peak_bytes = 0 : i64, "
        f"elided_bytes = {16 if candidate else 0} : i64, "
        f"elided_descriptor_count = {4 if candidate else 0} : i64, "
        f"elided_input_bytes = {8 if candidate else 0} : i64, "
        f"elided_input_descriptor_count = {2 if candidate else 0} : i64, "
        f"elided_input_request_count = {2 if candidate else 0} : i64, "
        f"elided_output_bytes = {8 if candidate else 0} : i64, "
        f"elided_output_descriptor_count = {2 if candidate else 0} : i64, "
        f"elided_output_request_count = {2 if candidate else 0} : i64, "
        f"elided_request_count = {4 if candidate else 0} : i64, "
        f"incremental_over_retained_control_input_bytes = {4 if candidate else 0} : i64, "
        f"incremental_over_retained_control_input_descriptor_count = {1 if candidate else 0} : i64, "
        f"incremental_over_retained_control_input_request_count = {1 if candidate else 0} : i64, "
        f"incremental_over_retained_control_output_bytes = {4 if candidate else 0} : i64, "
        f"incremental_over_retained_control_output_descriptor_count = {1 if candidate else 0} : i64, "
        f"incremental_over_retained_control_output_request_count = {1 if candidate else 0} : i64, "
        "explicit_assembly_edge_count = 0 : i64, "
        "explicit_reduction_edge_count = 0 : i64, fill_bytes = 4 : i64, "
        "fill_descriptor_count = 1 : i64, fill_request_count = 1 : i64, "
        "internal_edge_count = 1 : i64, materialized_shadow_edge_count = 0 : i64, "
        "member_count = 3 : i64, "
        f"member_task_count = {1 if candidate else 3} : i64, "
        f"owner_alias_edge_count = {0 if candidate else 1} : i64, "
        "physical_tile_id = 0 : i64, protected_spill_count = 1 : i64, "
        "region_id = 0 : i64, schema_version = 2 : i64, "
        f"ssa_internal_edge_count = {1 if candidate else 0} : i64, "
        "spill_bytes = 4 : i64, spill_descriptor_count = 1 : i64, "
        "spill_request_count = 1 : i64}"
    )
    physical_edge = (
        "{boundary_ids = [7], "
        f"disposition = \"{'ssa_internal' if candidate else 'owner_alias'}\", "
        "edge_ordinal = 0 : i64, fill_bytes = 0 : i64, "
        "fill_descriptor_count = 0 : i64, fill_request_count = 0 : i64, "
        "region_id = 0 : i64, schema_version = 2 : i64, "
        "spill_bytes = 0 : i64, spill_descriptor_count = 0 : i64, "
        "spill_request_count = 0 : i64}"
    )
    return (
        "module attributes {"
        + scalar
        + "sculptor.execution_residency.physical_certificate_version = 2 : i64, "
        + f"sculptor.execution_residency.physical_certificates = [{physical_region}], "
        + f"sculptor.execution_residency.physical_edge_certificates = [{physical_edge}], "
        + "sculptor.materialization.dma_descriptors = ["
        + ", ".join(selected_descriptors)
        + "]} {}\n"
    )


def materialization_counters(candidate: bool) -> dict[str, int]:
    counters = {
        "epoch_count": 3,
        "materialized_consumer_region_count": 3,
        "materialized_producer_region_count": 3,
        "materialized_tensor_count": 3,
        "materialized_tensor_bytes": 12,
        "materialized_main_transfer_count_logical": 4,
        "materialized_tail_transfer_count_logical": 0,
        # These are post-contraction implementation counters, not semantic
        # work.  A successfully contracted region no longer participates in
        # the retained-local planner.
        "retained_local_eligible_component_count": 0 if candidate else 1,
        "retained_local_selected_component_count": 0 if candidate else 1,
        "retained_local_fallback_component_count": 0,
        "preserved_external_spill_descriptor_count": 1,
        "preserved_hybrid_spill_descriptor_count": 0,
        "cross_epoch_direct_route_count": 0,
        "deferred_dependency_count": 0,
        "multiply_owned_materialized_byte_count": 0,
        "read_before_produced_region_count": 0,
        "unclassified_boundary_count": 0,
        "unowned_materialized_byte_count": 0,
        "zero_contribution_consumer_region_count": 0,
    }
    physical = 1 if candidate else 2
    counters.update(
        {
            "materialized_input_dma_descriptor_count": physical,
            "materialized_input_physical_request_count": physical,
            "materialized_input_physical_byte_count": physical * 4,
            "materialized_output_dma_descriptor_count": physical,
            "materialized_output_physical_request_count": physical,
            "materialized_output_physical_byte_count": physical * 4,
        }
    )
    return counters


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.control = root / "control"
        self.candidate = root / "candidate"
        self.control_sst = [root / f"control-sst-{index}" for index in range(3)]
        self.candidate_sst = [root / f"candidate-sst-{index}" for index in range(3)]
        self.make_compile(self.control, False)
        self.make_compile(self.candidate, True)
        for index, path in enumerate(self.control_sst):
            self.make_sst(path, self.control, False, 10_000_000.0, 10.0 + index)
        for index, path in enumerate(self.candidate_sst):
            self.make_sst(path, self.candidate, True, 8_000_000.0, 9.0 + 0.5 * index)

    def make_compile(self, run: Path, candidate: bool) -> None:
        (run / "deployment").mkdir(parents=True)
        (run / "cores").mkdir()
        (run / "active-cores.txt").write_text("0\n", encoding="utf-8")
        (run / "model.mlir").write_text("module { func.func @model() }\n", encoding="utf-8")
        for stage in STAGES:
            (run / "deployment" / stage).write_text(f"module {{ // {stage}\n}}\n", encoding="utf-8")
        write_json(
            run / "streaming-architecture.json",
            {
                "schema": "golem.streaming-architecture",
                "schema_version": 1,
                "source_mlir": str(
                    (run / "deployment" / "08-placed.mlir").resolve()
                ),
                "architecture": {
                    "version": 1,
                    "fixed_shard_bytes": 4096,
                    "scratchpad_bytes": 2097152,
                    "global_ram_bytes": 34359738368,
                    "max_in_flight": 2,
                    "noc_word_bytes": 4,
                },
            },
        )
        (run / "cores" / "core-0-finalized.mlir").write_text(core_module(candidate), encoding="utf-8")
        (run / "tile-0.elf").write_bytes(b"ELF-fixture")
        (run / "abi-preflight-summary.txt").write_text(
            "active_tiles=1\npasses=1\nepoch_count=3\nerrors=0\n", encoding="utf-8"
        )
        write_json(
            run / "deployment-manifest.json",
            {"schema": "sculptor.deployment", "version": 2, "active_tile_ids": [0]},
        )
        audit = {
            "schema": "sculptor.execution-residency-audit",
            "schema_version": 1,
            "status": "PASS",
            "mode": "select",
            "phase": 3,
            "selected_region_count": 1,
            "selected_member_count": 3,
            "selected_internal_edge_count": 1,
            "source_ir_sha256": "fixture-source",
        }
        write_json(run / "execution-residency-audit.json", audit)
        write_json(
            run / "materialization-audit.json",
            {
                "schema": "sculptor.materialization-audit",
                "version": 1,
                "status": "PASS",
                "active_tile_ids": [0],
                "counters": materialization_counters(candidate),
                "errors": [],
            },
        )
        qualification = {
            "schema": "sculptor.compile-qualification",
            "version": 1,
            "status": "PASS",
            "model": "fixture",
            "active_tile_ids": [0],
            "configuration": {"contract_scalar_execution_regions": candidate},
            "checks": {name: {"status": "PASS"} for name in CHECKS},
            "features": {},
            "errors": [],
        }
        qualification["checks"]["optimization_contracts"].update(
            {
                "scalar_contraction_enabled": candidate,
                "scalar_contracted_region_count": 1 if candidate else 0,
                "scalar_fallback_region_count": 0,
                "scalar_removed_kernel_count": 2 if candidate else 0,
                "scalar_region_record_count": 1 if candidate else 0,
            }
        )
        write_json(run / "compile-qualification.json", qualification)
        manifest = {
            "schema": "golem.sculptor-run",
            "run": {"id": run.name, "mode": "compile", "model": "fixture"},
            "environment": {
                "GOLEM_MODEL_OUTPUT_DIR": str(run),
                "GOLEM_MODEL_RUN_ID": run.name,
                "GOLEM_MODEL_CONTRACT_SCALAR_EXECUTION_REGIONS": "1" if candidate else "0",
                "GOLEM_MODEL_FIXED_SHARD_BYTES": "4096",
            },
            "parameters": {
                "execution_residency_mode": "select",
                "contract_scalar_execution_regions": "1" if candidate else "0",
                "configured_digital_workers": "16",
            },
            "hardware": {"mesh_rows": 1, "mesh_columns": 1, "arrays_per_core": 1},
            "source_trees": {"main": {"head": "same", "state_sha256": "same"}},
            "tools": {"compiler": {"sha256": "same", "bytes": 1}},
            "artifacts": {
                "compile_qualification": {"sha256": digest(run / "compile-qualification.json")},
                "execution_residency_audit": {"sha256": digest(run / "execution-residency-audit.json")},
                "materialization_audit": {"sha256": digest(run / "materialization-audit.json")},
            },
        }
        write_json(run / "run-manifest.json", manifest)

    def refresh_materialization_binding(self, run: Path) -> None:
        manifest_path = run / "run-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["artifacts"]["materialization_audit"]["sha256"] = digest(
            run / "materialization-audit.json"
        )
        write_json(manifest_path, manifest)

    def make_sst(
        self,
        evidence: Path,
        compile_run: Path,
        candidate: bool,
        simulated_ns: float,
        wall_seconds: float,
    ) -> None:
        evidence.mkdir()
        requests = 2 if candidate else 4
        byte_count = 8 if candidate else 16
        result_header = (
            "model,status,active_tiles,epoch_count,synchronization_mode,"
            "simulated_time,simulation_wall_seconds\n"
        )
        (evidence / "result.csv").write_text(
            result_header
            + f"fixture,PASS,1,3,exact_dependencies,{simulated_ns / 1e6:g} ms,{wall_seconds:g}\n",
            encoding="utf-8",
        )
        (evidence / "status.csv").write_text("status,exit_code\nPASS,0\n", encoding="utf-8")
        launch = {
            "schema": "golem.sculptor-sst-launch",
            "source_compile_directory": str(compile_run.resolve()),
            "source_run_manifest_sha256": digest(compile_run / "run-manifest.json"),
            "materialization_audit_sha256": digest(compile_run / "materialization-audit.json"),
            "model": "fixture",
            "sst_threads": 2,
            "sst_partitioner": "sst.simple",
            "global_ram_channels": 2,
            "synchronization_mode": "exact_dependencies",
            "sync_instruction_quantum": 1000,
            "physical_global_dma_work": {
                "input_requests": requests // 2,
                "output_requests": requests // 2,
                "tile_requests": {"0": requests},
            },
            "sst_work_partition": {"fixture": candidate},
            "runtime_tools": {
                "qemu": {
                    "resolved_path": "/fixture/bin/qemu",
                    "sha256": "1" * 64,
                },
                "progress_classifier": {
                    "resolved_path": str(
                        (evidence / "tools" / "classify.py").resolve()
                    ),
                    "sha256": "2" * 64,
                },
                "sst_partition_validator": {
                    "resolved_path": str(
                        (evidence / "tools" / "validate.py").resolve()
                    ),
                    "sha256": "3" * 64,
                },
            },
        }
        write_json(evidence / "launch.json", launch)
        issued = 2 if candidate else 4
        terminal = (
            "SCULPTOR_RA_INIT_PASS tile=0\n"
            "SCULPTOR_RA_PROGRESS kind=2 tile=0 "
            f"issued={issued} retired={issued} "
            f"physical_global_ram_dma_submitted={requests} "
            f"physical_global_ram_dma_completed={requests} "
            "current_epoch=3 earliest_incomplete_epoch=4294967295 "
            "active_count=0 active_loop=4294967295 next_issue=18446744073709551615 "
            "ready_queue=0 pending_receive=0 pending_transmit=0 pending_dma=0 "
            "active_receive_states=0 reported_receive_states=0\n"
            "SCULPTOR_RA_SIM_PASS tile=0\n"
            "MITTENS_QEMU_CAPTURE_HOST stop_reason=analog-submit(4) count=3\n"
            "MITTENS_QEMU_CAPTURE_HOST stop_reason=guest-exit(6) count=1\n"
        )
        (evidence / "simulation.log").write_text(terminal, encoding="utf-8")
        (evidence / "router-statistics.csv").write_text(
            "ComponentName,StatisticName,Sum.u64\n"
            f"global_ram,requests,{requests}\n"
            f"global_ram,bytes,{byte_count}\n",
            encoding="utf-8",
        )

    def command(self, control_sst: list[Path] | None = None) -> list[str]:
        command = [
            "python3",
            str(VALIDATOR),
            "--control-compile",
            str(self.control),
            "--candidate-compile",
            str(self.candidate),
        ]
        for path in control_sst if control_sst is not None else self.control_sst:
            command.extend(("--control-sst", str(path)))
        for path in self.candidate_sst:
            command.extend(("--candidate-sst", str(path)))
        command.extend(("--output", str(self.root / "report.json")))
        return command


class Phase5DifferentialValidatorTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.fixture = Fixture(Path(temporary.name))

    def run_validator(self, command: list[str] | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            command or self.fixture.command(), text=True, capture_output=True, check=False
        )

    def test_complete_same_tree_three_run_differential_passes(self) -> None:
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.fixture.root / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "PASS")
        certificate = report["compile"]["scalar_certificate"]
        self.assertEqual(certificate["contracted_region_count"], 1)
        self.assertEqual(
            certificate["certified_structural_elision"]["input"]["bytes"],
            8,
        )
        self.assertEqual(
            certificate["certified_incremental_over_retained_control"]["input"][
                "bytes"
            ],
            4,
        )
        self.assertEqual(report["compile"]["work"]["total_bytes"], {"control": 16, "candidate": 8})
        self.assertEqual(report["sst"]["logical_completion"]["analog_commands_completed"], 3)

    def test_pre_outline_stage_drift_fails_closed(self) -> None:
        (self.fixture.candidate / "deployment" / "08-placed.mlir").write_text(
            "module { // drift\n}\n", encoding="utf-8"
        )
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pre-outline stage 08-placed.mlir differs", result.stderr)

    def test_streaming_architecture_semantic_drift_fails_closed(self) -> None:
        path = self.fixture.candidate / "streaming-architecture.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["architecture"]["scratchpad_bytes"] += 1
        write_json(path, manifest)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("streaming architecture contracts differ", result.stderr)

    def test_certificate_without_observed_physical_delta_fails_closed(self) -> None:
        path = self.fixture.candidate / "materialization-audit.json"
        audit = json.loads(path.read_text(encoding="utf-8"))
        audit["counters"]["materialized_input_physical_byte_count"] = 8
        write_json(path, audit)
        self.fixture.refresh_materialization_binding(self.fixture.candidate)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not equal scalar certificate", result.stderr)

    def test_stale_incremental_certificate_field_fails_closed(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        field = "incremental_over_retained_control_input_bytes = 4 : i64"
        self.assertEqual(text.count(field), 2)
        path.write_text(text.replace(field, field.replace(" = 4 ", " = 5 "), 1), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incremental-over-retained-control certificate drifted", result.stderr)

    def test_stale_structural_certificate_field_fails_closed(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        field = "pre_contraction_input_bytes = 8 : i64"
        self.assertEqual(text.count(field), 1)
        path.write_text(text.replace(field, "pre_contraction_input_bytes = 9 : i64"), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("input bytes elision certificate drifted", result.stderr)

    def test_missing_incremental_certificate_field_fails_closed(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        field = "incremental_over_retained_control_output_request_count = 1 : i64, "
        self.assertEqual(text.count(field), 2)
        path.write_text(text.replace(field, "", 1), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lacks incremental_over_retained_control_output_request_count", result.stderr)

    def test_zero_incremental_region_is_valid_but_cannot_claim_aggregate_delta(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        for direction in ("input", "output"):
            for metric in ("descriptor_count", "request_count", "bytes"):
                name = f"incremental_over_retained_control_{direction}_{metric}"
                text, replacements = re.subn(
                    rf"({name} = )[14]( : i64)",
                    r"\g<1>0\g<2>",
                    text,
                )
                self.assertEqual(replacements, 2)
        path.write_text(text, encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("observed input descriptors delta 1 does not equal scalar certificate 0", result.stderr)

    def test_internal_physical_edge_work_fails_closed(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        old = "edge_ordinal = 0 : i64, fill_bytes = 0 : i64"
        self.assertEqual(text.count(old), 1)
        path.write_text(text.replace(old, "edge_ordinal = 0 : i64, fill_bytes = 1 : i64"), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nonzero internal physical RAM/NoC work", result.stderr)

    def test_cut_or_protected_descriptor_removal_fails_closed(self) -> None:
        path = self.fixture.candidate / "cores" / "core-0-finalized.mlir"
        text = path.read_text(encoding="utf-8")
        external = descriptor(
            1,
            22,
            22,
            102,
            1,
            (1 << 32) - 1,
            flags=2,
            epoch=2,
            port=3,
            loop=7,
            segment_offset=9,
        )
        internal = descriptor(1, 10, 10, 101, 1, 7)
        self.assertEqual(text.count(external), 1)
        path.write_text(text.replace(external, internal), encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cut/protected physical work", result.stderr)

    def test_fewer_than_three_clean_runs_fails_closed(self) -> None:
        result = self.run_validator(self.fixture.command(self.fixture.control_sst[:2]))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly three clean SST runs per arm", result.stderr)

    def test_runtime_tool_content_drift_fails_closed(self) -> None:
        path = self.fixture.candidate_sst[0] / "launch.json"
        launch = json.loads(path.read_text(encoding="utf-8"))
        launch["runtime_tools"]["progress_classifier"]["sha256"] = "4" * 64
        write_json(path, launch)
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SST launch policy differs", result.stderr)

    def test_analog_completion_mismatch_fails_closed(self) -> None:
        for evidence in self.fixture.candidate_sst:
            path = evidence / "simulation.log"
            path.write_text(
                path.read_text(encoding="utf-8").replace("analog-submit(4) count=3", "analog-submit(4) count=2"),
                encoding="utf-8",
            )
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("analog command completion count differs", result.stderr)

    def test_wall_median_regression_fails_closed(self) -> None:
        for evidence in self.fixture.candidate_sst:
            path = evidence / "result.csv"
            lines = path.read_text(encoding="utf-8").splitlines()
            fields = lines[1].split(",")
            fields[-1] = "13"
            path.write_text(lines[0] + "\n" + ",".join(fields) + "\n", encoding="utf-8")
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wall median regressed", result.stderr)

    def test_missing_terminal_pass_fails_closed(self) -> None:
        path = self.fixture.candidate_sst[0] / "simulation.log"
        path.write_text(
            path.read_text(encoding="utf-8").replace("SCULPTOR_RA_SIM_PASS tile=0\n", ""),
            encoding="utf-8",
        )
        result = self.run_validator()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("terminal tile accounting is incomplete", result.stderr)


if __name__ == "__main__":
    unittest.main()
