#!/usr/bin/env python3

import csv
import contextlib
import importlib.util
import io
import hashlib
import json
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
COLLECTOR = PROJECT_ROOT / "tools" / "analysis" / "collect-sculptor-model-suite-report.py"
SPEC = importlib.util.spec_from_file_location("model_suite_report", COLLECTOR)
REPORT = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(REPORT)


RUN_TAG = "fixture"


def write_csv(path, fieldnames, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def make_resnet32(root):
    run = root / "resnet32" / RUN_TAG
    run.mkdir(parents=True)
    write_csv(
        run / "status.csv",
        ["case", "status", "exit_code", "failure_stage", "total_wall_seconds"],
        [{"case": "resnet32", "status": "PASS", "exit_code": "0",
          "failure_stage": "", "total_wall_seconds": "12.5"}],
    )
    make_compile_evidence(run, "resnet32")
    for name in (
        "compile-qualification.json",
        "run-manifest.json",
        "harness-status.csv",
    ):
        (run / name).unlink()


def make_compile_evidence(run, model):
    configuration = {
        "fixed_shard_bytes": 4096,
        "supported_frame_bytes": [
            4096, 8192, 16384, 32768, 65536, 131072, 262144
        ],
        "max_in_flight": 2,
        "digital_workers": 16,
        "execution_residency_mode": "analyze",
        "execution_residency_maximum_members": 8,
        "execution_residency_maximum_wave_width": 2,
        "execution_residency_require_positive_benefit": True,
        "retained_local_policy_required": True,
        "direct_forward_policy_required": True,
        "persistent_matrix_policy_required": True,
        "exact_dependencies_required": True,
        "retained_owner_boundary_ids": "",
        "output_value_validation_required": False,
    }
    fingerprint = hashlib.sha256(
        json.dumps(configuration, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    checks = {
        name: {"status": "PASS"}
        for name in (
            "complete_lowering",
            "execution_residency_audit",
            "materialization_audit",
            "abi_preflight",
            "elf_generation",
            "memory_validation",
            "optimization_contracts",
        )
    }
    checks["abi_preflight"]["passes"] = 1
    checks["elf_generation"]["tile_elf_count"] = 1
    checks["execution_residency_audit"].update(
        {
            "mode": "analyze",
            "phase": 0,
            "physical_change_count": 0,
            "audit_sha256": "a" * 64,
        }
    )
    features = {
        "residency": {
            "contract_status": "PASS",
            "activated": True,
            "selected_component_count": 1,
        },
        "direct_forwarding": {
            "contract_status": "PASS",
            "activated": False,
            "selected_route_count": 0,
        },
        "persistent_matrices": {
            "contract_status": "PASS",
            "activated": True,
            "persistent_matrix_count": 1,
        },
        "circular_double_buffering": {
            "contract_status": "PASS",
            "activated": True,
            "double_buffering_activated": True,
            "single_slot_record_count": 1,
            "double_slot_record_count": 1,
        },
        "digital_workers": {
            "contract_status": "PASS",
            "activated": True,
            "configured_worker_limit": 16,
            "expanded_operation_count": 2,
            "expanded_work_unit_count": 4,
        },
        "exact_dependencies_local_epoch": {
            "contract_status": "PASS",
            "activated": True,
            "synchronization_mode": "exact_dependencies",
            "semantic_epoch_count": 3,
            "local_epoch_task_count": 2,
            "runtime_progress_observed": False,
        },
        "configurable_frames": {
            "contract_status": "PASS",
            "activated": True,
            "configured_maximum_frame_bytes": 4096,
            "supported_maximum_frame_bytes": configuration[
                "supported_frame_bytes"
            ],
            "observed_maximum_frame_bytes": [4096],
        },
    }
    certificate = {
        "schema": "sculptor.compile-qualification",
        "version": 1,
        "status": "PASS",
        "model": model,
        "active_tile_ids": [0],
        "configuration": configuration,
        "configuration_sha256": fingerprint,
        "checks": checks,
        "features": features,
        "output_value_validation_required": False,
        "errors": [],
    }
    certificate_path = run / "compile-qualification.json"
    certificate_path.write_text(json.dumps(certificate), encoding="utf-8")
    certificate_hash = hashlib.sha256(certificate_path.read_bytes()).hexdigest()
    required_artifacts = {
        name: {}
        for name in (
            "model_mlir",
            "expected_outputs",
            "deployment",
            "core_objects",
            "active_cores",
            "deployment_manifest",
            "abi_preflight_summary",
            "abi_preflight_logs",
            "materialization_audit",
            "execution_residency_audit",
            "memory_reports",
            "idle_elf",
            "tile_0_elf",
        )
    }
    required_artifacts["compile_qualification"] = {
        "kind": "file",
        "path": str(certificate_path),
        "resolved_path": str(certificate_path.resolve()),
        "sha256": certificate_hash,
    }
    (run / "run-manifest.json").write_text(
        json.dumps(
            {
                "schema": "golem.sculptor-run",
                "schema_version": 1,
                "run": {"model": model, "mode": "compile"},
                "artifacts": required_artifacts,
                "environment": {
                    "GOLEM_MODEL_FIXED_SHARD_BYTES": "4096",
                    "GOLEM_MODEL_MAX_IN_FLIGHT": "2",
                    "GOLEM_MODEL_DIGITAL_WORKERS": "16",
                    "GOLEM_MODEL_RETAIN_PROVED_LOCAL_OWNERS": "1",
                    "GOLEM_MODEL_EXECUTION_RESIDENCY_REGIONS": "analyze",
                    "GOLEM_MODEL_EXECUTION_RESIDENCY_MAXIMUM_MEMBERS": "8",
                    "GOLEM_MODEL_EXECUTION_RESIDENCY_MAXIMUM_WAVE_WIDTH": "2",
                    "GOLEM_MODEL_EXECUTION_RESIDENCY_REQUIRE_POSITIVE_BENEFIT": "1",
                },
            }
        ),
        encoding="utf-8",
    )
    write_csv(
        run / "harness-status.csv",
        [
            "model",
            "run_mode",
            "wrapper_status",
            "command_exit_code",
            "failure_stage",
            "wall_seconds",
            "timeout_seconds",
        ],
        [
            {
                "model": model,
                "run_mode": "compile",
                "wrapper_status": "COMPLETE",
                "command_exit_code": "0",
                "failure_stage": "",
                "wall_seconds": "1.0",
                "timeout_seconds": "10",
            }
        ],
    )
    (run / "resource-usage.csv").write_text("12.7,1234\n", encoding="utf-8")
    write_csv(
        run / "stage-timings.csv",
        ["stage", "wall_seconds"],
        [{"stage": "model_export", "wall_seconds": "1.0"},
         {"stage": "runtime_build", "wall_seconds": "2.0"},
         {"stage": "compiler_lowering", "wall_seconds": "3.0"},
         {"stage": "simulation", "wall_seconds": "100.0"}],
    )
    (run / "active-cores.txt").write_text("0\n1\n", encoding="utf-8")
    (run / "tile-0.elf").write_bytes(b"elf0")
    (run / "tile-1.elf").write_bytes(b"elf-one")
    write_csv(
        run / "result.csv",
        [
            "case",
            "mesh",
            "active_tiles",
            "simulated_time",
            "wall_seconds",
            "synchronization_mode",
            "output_validation",
        ],
        [{"case": "resnet32", "mesh": "1x2", "active_tiles": "2",
          "simulated_time": "5 ns", "wall_seconds": "0.4",
          "synchronization_mode": "exact_dependencies",
          "output_validation": "SKIPPED"}],
    )
    (run / "simulation.log").write_text(
        "Simulation is complete\nSCULPTOR_RA_SIM_PASS\n"
        "SCULPTOR_RA_SIM_PASS\nSCULPTOR_MODEL_OUTPUT tile=0 index=0\n",
        encoding="utf-8",
    )
    (run / "output-validation.json").write_text(
        json.dumps({"status": "PASS", "outputs": [
            {"index": 0, "comparison": "exact"}]}),
        encoding="utf-8",
    )
    (run / "materialization-audit.json").write_text(
        json.dumps(
            {
                "schema": "sculptor.materialization-audit",
                "version": 1,
                "status": "PASS",
                "maximum_frame_bytes": 4096,
                "counters": {
                    field: 0 for field in REPORT.MATERIALIZATION_COUNTER_FIELDS
                },
            }
        ),
        encoding="utf-8",
    )
    write_csv(
        run / "router-statistics.csv",
        ["ComponentName", "StatisticName", "StatisticSubId",
         "SimTime", "Sum.u64"],
        [{"ComponentName": "global_ram", "StatisticName": "requests",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "3"},
         {"ComponentName": "global_ram", "StatisticName": "bytes",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "8192"},
         {"ComponentName": "global_ram", "StatisticName": "readiness_delay_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "5"},
         {"ComponentName": "global_ram", "StatisticName": "queue_delay_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "7"},
         {"ComponentName": "global_ram", "StatisticName": "service_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "11"},
         {"ComponentName": "global_ram", "StatisticName": "execution_teardown_wait_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "13"},
         {"ComponentName": "tile0", "StatisticName": "scratchpad_service_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "17"},
         {"ComponentName": "memory_init_barrier", "StatisticName": "barrier_wait_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "19"},
         {"ComponentName": "memory_init_barrier", "StatisticName": "tile_wait_cycles",
          "StatisticSubId": "", "SimTime": "5000", "Sum.u64": "23"},
         {"ComponentName": "router_0_0", "StatisticName": "flits_forwarded",
          "StatisticSubId": "east", "SimTime": "5000", "Sum.u64": "10"},
         {"ComponentName": "router_0_0", "StatisticName": "flits_forwarded",
          "StatisticSubId": "local", "SimTime": "5000", "Sum.u64": "100"},
         {"ComponentName": "router_0_0", "StatisticName": "output_credit_stall_cycles",
          "StatisticSubId": "east", "SimTime": "5000", "Sum.u64": "2"}],
    )
    write_csv(
        run / "trace" / "performance" / "tile-0-summary.csv",
        ["metric", "value"],
        [{"metric": "wait_analog-submit_ticks", "value": "13"},
         {"metric": "wait_scratchpad-dma-wait_ticks", "value": "17"},
         {"metric": "wait_nic-receive-wait_ticks", "value": "19"},
         {"metric": "wait_memory-access_ticks", "value": "23"}],
    )


def main():
    with tempfile.TemporaryDirectory(prefix="model-suite-report-") as directory:
        root = Path(directory)
        make_resnet32(root)
        csv_path = root / "full-model-summary.csv"
        json_path = root / "full-model-summary.json"
        rows = [
            REPORT.collect_row(root / model / RUN_TAG, model, "1GHz")
            for model in REPORT.REQUIRED_MODELS
        ]
        assert REPORT.write_report(rows, csv_path, json_path, strict=False)
        report_rows = list(csv.DictReader(csv_path.open(encoding="utf-8")))
        assert [row["model"] for row in report_rows] == list(REPORT.REQUIRED_MODELS)
        row = report_rows[0]
        assert row["compiler_wall_seconds"] == "6.0"
        assert row["elf_bytes_total"] == "11"
        assert row["simulated_cycles"] == "5"
        assert row["maximum_frame_bytes"] == "4096"
        assert row["synchronization_mode"] == "exact_dependencies"
        assert row["output_validation"] == "SKIPPED"
        assert row["ram_dma_bytes"] == "8192"
        assert row["ram_dma_readiness_delay_cycles"] == "5"
        assert row["ram_dma_queue_delay_cycles"] == "7"
        assert row["ram_dma_service_cycles"] == "11"
        assert row["ram_execution_teardown_wait_cycles"] == "13"
        assert row["scratchpad_service_cycles"] == "17"
        assert row["initialization_barrier_wait_cycles"] == "19"
        assert row["initialization_tile_wait_cycles"] == "23"
        assert row["noc_flits"] == "10"
        assert row["noc_stall_cycles"] == "2"
        assert row["compute_stall_ticks"] == "13"
        assert row["spm_stall_ticks"] == "23"
        assert row["noc_stall_ticks"] == "19"
        assert row["ram_dma_stall_ticks"] == "17"
        assert row["exact_output_status"] == "PASS"
        assert row["sim_pass_status"] == "PASS"
        assert report_rows[1]["status"] == "NOT_REACHED"

        payload = json.loads(json_path.read_text(encoding="utf-8"))
        assert payload["validation"]["missing_models"] == list(
            REPORT.REQUIRED_MODELS[1:]
        )
        assert not payload["validation"]["exact_outputs_required"]
        with contextlib.redirect_stderr(io.StringIO()):
            assert not REPORT.write_report(rows, csv_path, json_path, strict=True)

        incomplete_run = root / "wide-resnet-16-8" / RUN_TAG
        incomplete_run.mkdir(parents=True)
        assert REPORT.collect_row(
            incomplete_run, "wide-resnet-16-8", "1GHz"
        )["status"] == "INCOMPLETE"
        timeout_run = root / "rnnt" / RUN_TAG
        timeout_run.mkdir(parents=True)
        write_csv(
            timeout_run / "harness-status.csv",
            [
                "model", "run_mode", "wrapper_status", "command_exit_code",
                "failure_stage", "wall_seconds", "timeout_seconds",
            ],
            [{
                "model": "rnnt", "run_mode": "compile",
                "wrapper_status": "TIMEOUT", "command_exit_code": "124",
                "failure_stage": "hard_wall_timeout", "wall_seconds": "10",
                "timeout_seconds": "10",
            }],
        )
        assert REPORT.collect_row(timeout_run, "rnnt", "1GHz")["status"] == "TIMEOUT"
        kill_after_run = root / "retinanet" / RUN_TAG
        kill_after_run.mkdir(parents=True)
        write_csv(
            kill_after_run / "harness-status.csv",
            [
                "model", "run_mode", "wrapper_status", "command_exit_code",
                "failure_stage", "wall_seconds", "timeout_seconds",
            ],
            [{
                "model": "retinanet", "run_mode": "compile",
                "wrapper_status": "TIMEOUT", "command_exit_code": "137",
                "failure_stage": "hard_wall_timeout", "wall_seconds": "20",
                "timeout_seconds": "10",
            }],
        )
        assert REPORT.collect_row(
            kill_after_run, "retinanet", "1GHz"
        )["status"] == "TIMEOUT"

        for model in REPORT.REQUIRED_MODELS:
            run = root / model / RUN_TAG
            run.mkdir(parents=True, exist_ok=True)
            write_csv(
                run / "status.csv",
                ["case", "status", "exit_code", "failure_stage",
                 "total_wall_seconds"],
                [{"case": model, "status": "COMPILE_PASS", "exit_code": "0",
                  "failure_stage": "", "total_wall_seconds": "1.0"}],
            )
            (run / "materialization-audit.json").write_text(
                json.dumps(
                    {
                        "schema": "sculptor.materialization-audit",
                        "version": 1,
                        "status": "PASS",
                        "counters": {
                            field: 0
                            for field in REPORT.MATERIALIZATION_COUNTER_FIELDS
                        },
                    }
                ),
                encoding="utf-8",
            )
            make_compile_evidence(run, model)
        compile_rows = [
            REPORT.collect_row(root / model / RUN_TAG, model, "1GHz")
            for model in REPORT.REQUIRED_MODELS
        ]
        assert REPORT.write_report(
            compile_rows,
            csv_path,
            json_path,
            strict=True,
            completion_mode="compile",
        )
        compile_payload = json.loads(json_path.read_text(encoding="utf-8"))
        assert compile_payload["validation"]["completion_mode"] == "compile"
        assert compile_payload["validation"]["all_models_compile"]
        assert compile_payload["validation"]["complete"]
        assert compile_payload["validation"]["qualification_ratio"] == "8/8"
        assert compile_payload["validation"]["uniform_feature_configuration"]
        assert compile_payload["validation"]["observed_maximum_frame_bytes"] == [
            4096
        ]
        assert compile_payload["validation"]["feature_coverage"][
            "direct_forwarding"
        ]["inactive_models"] == list(REPORT.REQUIRED_MODELS)
        assert not compile_payload["validation"]["activation_coverage_complete"]
        assert compile_payload["validation"]["never_activated_features"] == [
            "direct_forwarding"
        ]

        victim = root / "rnnt" / RUN_TAG / "harness-status.csv"
        saved_harness = victim.read_text(encoding="utf-8")
        write_csv(
            victim,
            [
                "model", "run_mode", "wrapper_status", "command_exit_code",
                "failure_stage", "wall_seconds", "timeout_seconds",
            ],
            [{
                "model": "rnnt", "run_mode": "compile",
                "wrapper_status": "TIMEOUT", "command_exit_code": "124",
                "failure_stage": "hard_wall_timeout", "wall_seconds": "10",
                "timeout_seconds": "10",
            }],
        )
        timeout_rows = [
            REPORT.collect_row(root / model / RUN_TAG, model, "1GHz")
            for model in REPORT.REQUIRED_MODELS
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            assert not REPORT.write_report(
                timeout_rows,
                csv_path,
                json_path,
                strict=True,
                completion_mode="compile",
            )
        timeout_payload = json.loads(json_path.read_text(encoding="utf-8"))
        assert timeout_payload["validation"]["timed_out_models"] == ["rnnt"]
        assert timeout_payload["validation"]["qualification_ratio"] == "7/8"
        victim.write_text(saved_harness, encoding="utf-8")

        certificate_path = root / "rnnt" / RUN_TAG / "compile-qualification.json"
        manifest_path = root / "rnnt" / RUN_TAG / "run-manifest.json"
        saved_certificate = certificate_path.read_text(encoding="utf-8")
        saved_manifest = manifest_path.read_text(encoding="utf-8")
        certificate = json.loads(saved_certificate)
        certificate["configuration"]["digital_workers"] = 8
        certificate["features"]["digital_workers"][
            "configured_worker_limit"
        ] = 8
        certificate["configuration_sha256"] = hashlib.sha256(
            json.dumps(
                certificate["configuration"],
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        certificate_path.write_text(json.dumps(certificate), encoding="utf-8")
        manifest = json.loads(saved_manifest)
        manifest["artifacts"]["compile_qualification"]["sha256"] = hashlib.sha256(
            certificate_path.read_bytes()
        ).hexdigest()
        manifest["environment"]["GOLEM_MODEL_DIGITAL_WORKERS"] = "8"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        nonuniform_rows = [
            REPORT.collect_row(root / model / RUN_TAG, model, "1GHz")
            for model in REPORT.REQUIRED_MODELS
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            assert not REPORT.write_report(
                nonuniform_rows,
                csv_path,
                json_path,
                strict=True,
                completion_mode="compile",
            )
        nonuniform_payload = json.loads(json_path.read_text(encoding="utf-8"))
        assert not nonuniform_payload["validation"][
            "uniform_feature_configuration"
        ]
        assert nonuniform_payload["validation"][
            "nonuniform_configuration_models"
        ] == list(REPORT.REQUIRED_MODELS)
        certificate_path.write_text(saved_certificate, encoding="utf-8")
        manifest_path.write_text(saved_manifest, encoding="utf-8")

        # Numerical output comparison is reported but is not a V1 completion
        # gate unless a caller explicitly opts into it.
        simulation_rows = []
        for model in REPORT.REQUIRED_MODELS:
            row = dict(compile_rows[REPORT.REQUIRED_MODELS.index(model)])
            row.update(
                status="PASS",
                materialization_audit_status="PASS",
                sim_pass_status="PASS",
                exact_output_status="NOT_RUN",
            )
            simulation_rows.append(row)
        assert REPORT.write_report(
            simulation_rows, csv_path, json_path, strict=True
        )
        skipped_payload = json.loads(json_path.read_text(encoding="utf-8"))
        assert skipped_payload["validation"]["complete"]
        assert skipped_payload["validation"]["non_exact_models"] == list(
            REPORT.REQUIRED_MODELS
        )
        with contextlib.redirect_stderr(io.StringIO()):
            assert not REPORT.write_report(
                simulation_rows,
                csv_path,
                json_path,
                strict=True,
                require_exact_outputs=True,
            )


if __name__ == "__main__":
    main()
