#!/usr/bin/env python3

import csv
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
VALIDATOR = PROJECT_ROOT / "scripts" / "validate-sculptor-compile-qualification.py"
ACTIVE_TILES = [0, 1]
LOWERING_STAGES = (
    "01-canonical",
    "02-converted",
    "03-layouts",
    "03-golem",
    "03-resolved-layouts",
    "04-expanded-digital-work",
    "04-parametric-work",
    "04-tensor-fragments",
    "04-residency-regions",
    "05-ra-tree",
    "06-mapping-plan",
    "08-placed",
    "09-tile-deployment-split",
)
LOWERING_FILES = (
    "01-canonical.mlir",
    "02-converted.mlir",
    "03-layouts.mlir",
    "03-golem.mlir",
    "03-resolved-layouts.mlir",
    "04-expanded-digital-work.mlir",
    "04-parametric-work.mlir",
    "04-tensor-fragments.mlir",
    "04-residency-regions.mlir",
    "05-ra-tree.mlir",
    "06-mapping-plan.mlir",
    "08-placed.mlir",
)
CORE_SUFFIXES = (
    "-extracted.mlir",
    "-runtime-graph.mlir",
    "-planned.mlir",
    "-finalized.mlir",
    "-compute-llvm.mlir",
    "-task-only.mlir",
    ".ll",
    ".o",
)


def write_csv(path, fieldnames, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_elf(path, machine=243):
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    header[18:20] = struct.pack("<H", machine)
    path.write_bytes(header)


def finalized_module(tile):
    ring_slots = 2 if tile == 0 else 1
    persistent_count = 1 if tile == 0 else 0
    return f'''module attributes {{
  sculptor.arch.streaming = {{fixed_shard_bytes = 4096 : i64, global_ram_bytes = 34359738368 : i64, max_in_flight = 2 : i64, noc_word_bytes = 4 : i64, scratchpad_bytes = 2097152 : i64, version = 1 : i64}},
  sculptor.mapping.digital_profitability_accepted_candidate_count = 2 : i64,
  sculptor.mapping.digital_profitability_legal_candidate_count = 2 : i64,
  sculptor.mapping.expanded_digital_operation_count = 2 : i64,
  sculptor.mapping.expanded_digital_work_unit_count = 4 : i64,
  sculptor.materialization.direct_forward_policy_enabled = true,
  sculptor.materialization.exact_ram_readiness_certificate = {{descriptor_count = 2 : i64, mode = "exact_dependencies", schema_version = 1 : i64}},
  sculptor.materialization.exact_ram_readiness_enabled = true,
  sculptor.residency.persistent_matrices = [],
  sculptor.residency.persistent_matrix_audit = {{persistent_matrix_count = {persistent_count} : i64}},
  sculptor.residency.persistent_matrix_version = 1 : i64,
  sculptor.residency.plan_version = 1 : i64,
  sculptor.residency.records = [#sculptor.shard_residency<id = 0 : i64, ringSlots = {ring_slots} : i64>],
  sculptor.residency.transitions = []
}} {{
  "test.task"() {{sculptor.materialization.execution_epoch_id = 1 : i64}} : () -> ()
}}
'''


def scalar_finalized_module(tile, *, zero_incremental=False):
    value = 0 if zero_incremental else 1
    byte_count = 0 if zero_incremental else 4
    scalar_record = ""
    physical_record = ""
    if tile == 0:
        scalar_record = (
            "{"
            f"incremental_over_retained_control_input_bytes = {byte_count} : i64, "
            f"incremental_over_retained_control_input_descriptor_count = {value} : i64, "
            f"incremental_over_retained_control_input_request_count = {value} : i64, "
            "incremental_over_retained_control_output_bytes = 0 : i64, "
            "incremental_over_retained_control_output_descriptor_count = 0 : i64, "
            "incremental_over_retained_control_output_request_count = 0 : i64, "
            "input_port_count = 1 : i64, internal_boundary_ids = [7], "
            "region_id = 7 : i64, "
            "schema_version = 3 : i64, wrapper_routine_id = 70 : i64}"
        )
        physical_record = (
            "{actual_region_owner_bytes = 4 : i64, "
            f"incremental_over_retained_control_input_bytes = {byte_count} : i64, "
            f"incremental_over_retained_control_input_descriptor_count = {value} : i64, "
            f"incremental_over_retained_control_input_request_count = {value} : i64, "
            "incremental_over_retained_control_output_bytes = 0 : i64, "
            "incremental_over_retained_control_output_descriptor_count = 0 : i64, "
            "incremental_over_retained_control_output_request_count = 0 : i64, "
            "region_id = 7 : i64, schema_version = 2 : i64}"
        )
    extra = f''',
  sculptor.execution_residency.physical_certificate_version = 2 : i64,
  sculptor.execution_residency.physical_certificates = [{physical_record}],
  sculptor.execution_residency.scalar_contracted_count = 1 : i64,
  sculptor.execution_residency.scalar_fallback_count = 0 : i64,
  sculptor.execution_residency.scalar_plan_version = 3 : i64,
  sculptor.execution_residency.scalar_regions = [{scalar_record}],
  sculptor.execution_residency.scalar_removed_kernel_count = 2 : i64'''
    return finalized_module(tile).replace("\n} {\n", extra + "\n} {\n", 1)


def load_validator_module():
    spec = importlib.util.spec_from_file_location(
        "sculptor_compile_qualification_validator", VALIDATOR
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def exercise_scalar_schema(parent):
    validator = load_validator_module()
    root = parent / "scalar-schema"
    cores = root / "cores"
    cores.mkdir(parents=True)
    for tile in ACTIVE_TILES:
        (cores / f"core-{tile}-finalized.mlir").write_text(
            scalar_finalized_module(tile), encoding="utf-8"
        )
    architecture = {"fixed_shard_bytes": 4096, "max_in_flight": 2}

    def validate():
        return validator.validate_tile_contracts(
            root, ACTIVE_TILES, architecture, 16, True
        )

    contracts, _, _, _ = validate()
    assert contracts["scalar_contracted_region_count"] == 1
    assert contracts["scalar_region_record_count"] == 1

    def expect_scalar_failure(name, mutate, message):
        fixture = parent / f"scalar-{name}"
        shutil.copytree(root, fixture)
        mutate(fixture / "cores" / "core-0-finalized.mlir")
        try:
            validator.validate_tile_contracts(
                fixture, ACTIVE_TILES, architecture, 16, True
            )
        except validator.QualificationError as error:
            assert error.check == "optimization_contracts"
            assert message in str(error), (name, str(error))
        else:
            raise AssertionError(f"{name} unexpectedly passed")

    def replace_once(path, old, new):
        text = path.read_text(encoding="utf-8")
        assert text.count(old) >= 1
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    expect_scalar_failure(
        "stale-version",
        lambda path: replace_once(
            path,
            "scalar_plan_version = 3 : i64",
            "scalar_plan_version = 2 : i64",
        ),
        "stale scalar contraction plan",
    )
    expect_scalar_failure(
        "missing-incremental",
        lambda path: replace_once(
            path,
            "incremental_over_retained_control_input_request_count = 1 : i64, ",
            "",
        ),
        "does not carry exactly one incremental_over_retained_control_input_request_count",
    )
    expect_scalar_failure(
        "incremental-drift",
        lambda path: replace_once(
            path,
            "incremental_over_retained_control_input_bytes = 4 : i64",
            "incremental_over_retained_control_input_bytes = 8 : i64",
        ),
        "incremental physical certificate drifted",
    )

    for tile in ACTIVE_TILES:
        (cores / f"core-{tile}-finalized.mlir").write_text(
            scalar_finalized_module(tile, zero_incremental=True),
            encoding="utf-8",
        )
    zero_contracts, _, _, _ = validate()
    assert zero_contracts["scalar_contracted_region_count"] == 1


def make_fixture(root):
    root.mkdir(parents=True)
    deployment = root / "deployment"
    cores = root / "cores"
    abi = root / "abi-preflight"
    memory = root / "memory-reports"
    for directory in (deployment, cores, abi, memory):
        directory.mkdir()
    (root / "active-cores.txt").write_text("0\n1\n", encoding="utf-8")
    for name in LOWERING_FILES:
        (deployment / name).write_text("module {}\n", encoding="utf-8")
    source_bytes = (deployment / "04-tensor-fragments.mlir").read_bytes()
    source_sha256 = hashlib.sha256(source_bytes).hexdigest()
    configuration_description = (
        "mode=analyze\n"
        "maximum_members=8\n"
        "maximum_wave_width=2\n"
        "require_positive_benefit=true\n"
    ).encode("utf-8")
    zero_fields = (
        "physical_change_count", "candidate_count", "selected_region_count",
        "selected_member_count", "selected_internal_edge_count",
        "analog_digital_region_count", "digital_only_region_count",
        "candidate_logical_bytes", "estimated_avoided_input_bytes",
        "estimated_avoided_output_bytes", "estimated_avoided_requests",
        "actual_elided_input_descriptors", "actual_elided_output_descriptors",
        "actual_elided_input_bytes", "actual_elided_output_bytes",
        "actual_elided_physical_requests", "preserved_external_spills",
        "preserved_remote_routes", "region_ring_bytes",
        "region_temporary_bytes", "scalar_region_invocations",
        "wave_region_invocations", "region_static_instructions",
        "region_dynamic_instructions", "region_dispatches",
        "analog_commands_submitted", "analog_commands_completed",
        "logical_internal_bytes", "physical_internal_bytes",
    )
    execution_audit = {
        "schema": "sculptor.execution-residency-audit",
        "schema_version": 1,
        "status": "PASS",
        "phase": 0,
        "plan_version": 1,
        "graph_fingerprint": source_sha256,
        "canonical_ir_bytes": len(source_bytes),
        "source_ir_sha256": source_sha256,
        "source_ir_bytes": len(source_bytes),
        "mode": "analyze",
        "configuration": {
            "maximum_members": 8,
            "maximum_wave_width": 2,
            "require_positive_benefit": True,
        },
        "configuration_sha256": hashlib.sha256(
            configuration_description
        ).hexdigest(),
        "semantic_ir_changed": False,
        "accounting_complete": False,
        "candidate_rejections_by_reason": {},
        "per_tile_peak_spm_bytes": {},
        "per_tile_headroom_bytes": {},
        "wave_width_histogram": {},
        "regions": [],
        "tiles": [],
    }
    execution_audit.update({field: 0 for field in zero_fields})
    (root / "execution-residency-audit.json").write_text(
        json.dumps(execution_audit), encoding="utf-8"
    )
    placed = (deployment / "08-placed.mlir").resolve()
    (root / "streaming-architecture.json").write_text(
        json.dumps(
            {
                "schema": "golem.streaming-architecture",
                "schema_version": 1,
                "source_mlir": str(placed),
                "architecture": {
                    "version": 1,
                    "fixed_shard_bytes": 4096,
                    "scratchpad_bytes": 2097152,
                    "global_ram_bytes": 34359738368,
                    "max_in_flight": 2,
                    "noc_word_bytes": 4,
                },
            }
        ),
        encoding="utf-8",
    )
    write_csv(
        deployment / "compiler-stage-metrics.csv",
        [
            "stage",
            "wall_seconds",
            "max_rss_kb",
            "output_bytes",
            "operation_count",
            "exit_code",
            "diagnostics",
            "crash_reproducer",
        ],
        [
            {
                "stage": stage,
                "wall_seconds": "0.01",
                "max_rss_kb": "1",
                "output_bytes": "1",
                "operation_count": "1",
                "exit_code": "0",
                "diagnostics": "fixture.log",
                "crash_reproducer": "NA",
            }
            for stage in LOWERING_STAGES
        ],
    )
    for tile in ACTIVE_TILES:
        for suffix in CORE_SUFFIXES:
            path = cores / f"core-{tile}{suffix}"
            if suffix == "-finalized.mlir":
                path.write_text(finalized_module(tile), encoding="utf-8")
            else:
                path.write_bytes(b"fixture\n")
    (root / "deployment-manifest.json").write_text(
        json.dumps(
            {
                "schema": "sculptor.deployment",
                "version": 2,
                "active_tile_ids": ACTIVE_TILES,
                "synchronization": {
                    "mode": "exact_dependencies",
                    "semantic_epoch_count": 3,
                },
            }
        ),
        encoding="utf-8",
    )
    counters = {
        "epoch_count": 3,
        "unowned_materialized_byte_count": 0,
        "multiply_owned_materialized_byte_count": 0,
        "read_before_produced_region_count": 0,
        "cross_epoch_direct_route_count": 0,
        "unclassified_boundary_count": 0,
        "phase4_accounting_tile_count": 2,
        "phase5_accounting_tile_count": 2,
        "retained_local_policy_enabled_tile_count": 2,
        "retained_local_eligible_component_count": 1,
        "retained_local_selected_component_count": 1,
        "retained_local_fallback_component_count": 0,
        "retained_local_route_count": 1,
        "elided_retained_input_descriptor_count": 1,
        "elided_retained_output_descriptor_count": 1,
        "direct_forward_selected_route_count": 0,
        "direct_forward_elided_input_descriptor_count": 0,
        "direct_forward_elided_output_descriptor_count": 0,
    }
    (root / "materialization-audit.json").write_text(
        json.dumps(
            {
                "schema": "sculptor.materialization-audit",
                "version": 1,
                "status": "PASS",
                "errors": [],
                "active_tile_ids": ACTIVE_TILES,
                "global_ram_bytes": 34359738368,
                "maximum_frame_bytes": 4096,
                "counters": counters,
                "tiles": [
                    {
                        "tile_id": tile,
                        "phase4_accounting_contract": True,
                        "phase5_accounting_contract": True,
                    }
                    for tile in ACTIVE_TILES
                ],
            }
        ),
        encoding="utf-8",
    )
    (root / "abi-preflight-summary.txt").write_text(
        "active_tiles=2\npasses=2\nepoch_count=3\nerrors=0\n"
        "timeout_seconds=10\nparallel_jobs=2\n",
        encoding="utf-8",
    )
    for tile in ACTIVE_TILES:
        (abi / f"tile-{tile}.log").write_text(
            f"SCULPTOR_RA_ABI_PASS tile={tile} epoch_count=3\n",
            encoding="utf-8",
        )
        write_elf(root / f"tile-{tile}.elf")
    write_elf(root / "idle.elf")

    tiles = {}
    for tile in ACTIVE_TILES:
        count = 1 if tile == 0 else 0
        tiles[str(tile)] = {
            "capacity": {"complete": True, "requiredLocalBytes": 4096},
            "finalized": {
                "persistent_matrix_count": count,
                "persistent_matrix_programmed_bytes": 4096 * count,
                "persistent_matrix_consumer_task_count": count,
                "persistent_matrix_consumer_execution_count": 2 * count,
                "persistent_matrix_maximum_last_use_epoch": 2 * count,
                "persistent_matrix_duplicate_setup_count": 0,
                "persistent_matrix_unproven_use_count": 0,
                "persistent_matrix_repeated_setup_count": 0,
            },
        }
    (memory / "tile-memory-summary.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "active_tile_count": 2,
                "scratchpad_capacity_bytes": 2097152,
                "capacity_gate": {"status": "PASS", "errors": []},
                "tiles": tiles,
            }
        ),
        encoding="utf-8",
    )


def command(run, output, *extra, frame=4096):
    return [
        "python3",
        str(VALIDATOR),
        "--run-directory",
        str(run),
        "--model",
        "fixture-model",
        "--expected-network-size",
        "4",
        "--expected-frame-bytes",
        str(frame),
        "--expected-max-in-flight",
        "2",
        "--expected-digital-workers",
        "16",
        "--expected-execution-residency-mode",
        "analyze",
        "--expected-execution-residency-maximum-members",
        "8",
        "--expected-execution-residency-maximum-wave-width",
        "2",
        "--expected-execution-residency-require-positive-benefit",
        "true",
        "--expected-contract-scalar-execution-regions",
        "true",
        "--output",
        str(output),
        *extra,
    ]


def expect_failure(base, parent, name, mutate, expected_check, *extra):
    fixture = parent / name
    shutil.copytree(base, fixture)
    architecture_path = fixture / "streaming-architecture.json"
    architecture = json.loads(architecture_path.read_text(encoding="utf-8"))
    architecture["source_mlir"] = str(
        (fixture / "deployment" / "08-placed.mlir").resolve()
    )
    architecture_path.write_text(json.dumps(architecture), encoding="utf-8")
    mutate(fixture)
    output = parent / f"{name}.json"
    result = subprocess.run(command(fixture, output, *extra), capture_output=True, text=True)
    assert result.returncode == 1, (name, result.stdout, result.stderr)
    payload = json.loads(output.read_text(encoding="utf-8"))
    assert payload["status"] == "FAIL"
    assert payload["failed_check"] == expected_check, payload


def main():
    with tempfile.TemporaryDirectory(prefix="compile-qualification-") as directory:
        parent = Path(directory)
        base = parent / "base"
        make_fixture(base)
        output = parent / "positive.json"
        subprocess.run(command(base, output), check=True)
        payload = json.loads(output.read_text(encoding="utf-8"))
        assert payload["status"] == "PASS"
        assert payload["output_value_validation_required"] is False
        assert payload["features"]["residency"]["activated"] is True
        assert payload["features"]["direct_forwarding"]["activated"] is False
        assert payload["features"]["persistent_matrices"]["activated"] is True
        assert payload["features"]["circular_double_buffering"][
            "double_buffering_activated"
        ] is True
        assert payload["features"]["digital_workers"]["activated"] is True
        assert payload["features"]["exact_dependencies_local_epoch"][
            "runtime_progress_observed"
        ] is False
        assert payload["features"]["configurable_frames"][
            "observed_maximum_frame_bytes"
        ] == [4096]
        assert payload["checks"]["execution_residency_audit"]["status"] == "PASS"
        assert len(payload["checks"]["execution_residency_audit"]["audit_sha256"]) == 64
        exercise_scalar_schema(parent)

        for frame in (8192, 16384, 32768, 65536, 131072, 262144):
            fixture = parent / f"frame-{frame}"
            shutil.copytree(base, fixture)
            architecture_path = fixture / "streaming-architecture.json"
            architecture = json.loads(
                architecture_path.read_text(encoding="utf-8")
            )
            architecture["source_mlir"] = str(
                (fixture / "deployment" / "08-placed.mlir").resolve()
            )
            architecture["architecture"]["fixed_shard_bytes"] = frame
            architecture_path.write_text(json.dumps(architecture), encoding="utf-8")
            audit_path = fixture / "materialization-audit.json"
            audit = json.loads(audit_path.read_text(encoding="utf-8"))
            audit["maximum_frame_bytes"] = frame
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            for tile in ACTIVE_TILES:
                finalized = fixture / "cores" / f"core-{tile}-finalized.mlir"
                finalized.write_text(
                    finalized.read_text(encoding="utf-8").replace(
                        "fixed_shard_bytes = 4096", f"fixed_shard_bytes = {frame}"
                    ),
                    encoding="utf-8",
                )
            frame_output = parent / f"frame-{frame}.json"
            subprocess.run(
                command(fixture, frame_output, frame=frame), check=True,
                capture_output=True, text=True,
            )
            frame_payload = json.loads(frame_output.read_text(encoding="utf-8"))
            assert frame_payload["features"]["configurable_frames"][
                "observed_maximum_frame_bytes"
            ] == [frame]

        expect_failure(
            base,
            parent,
            "missing-lowering",
            lambda run: (run / "cores" / "core-1.ll").unlink(),
            "complete_lowering",
        )
        expect_failure(
            base,
            parent,
            "stale-execution-residency-audit",
            lambda run: (run / "execution-residency-audit.json").write_text(
                (run / "execution-residency-audit.json")
                .read_text(encoding="utf-8")
                .replace('"maximum_members": 8', '"maximum_members": 9'),
                encoding="utf-8",
            ),
            "execution_residency_audit",
        )
        expect_failure(
            base,
            parent,
            "incomplete-abi",
            lambda run: (run / "abi-preflight" / "tile-1.log").unlink(),
            "abi_preflight",
        )
        expect_failure(
            base,
            parent,
            "wrong-elf",
            lambda run: write_elf(run / "tile-1.elf", machine=62),
            "elf_generation",
        )

        def remove_direct(run):
            path = run / "cores" / "core-1-finalized.mlir"
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "sculptor.materialization.direct_forward_policy_enabled = true,",
                    "",
                ),
                encoding="utf-8",
            )

        expect_failure(
            base,
            parent,
            "missing-feature-contract",
            remove_direct,
            "optimization_contracts",
        )

        def corrupt_frame(run):
            path = run / "streaming-architecture.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["architecture"]["fixed_shard_bytes"] = 8192
            path.write_text(json.dumps(value), encoding="utf-8")

        expect_failure(
            base,
            parent,
            "frame-disagreement",
            corrupt_frame,
            "configurable_frames",
        )
        expect_failure(
            base,
            parent,
            "model-specific-filter",
            lambda run: None,
            "configuration",
            "--retained-owner-boundary-ids",
            "17",
        )


if __name__ == "__main__":
    main()
