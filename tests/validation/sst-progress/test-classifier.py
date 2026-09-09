#!/usr/bin/env python3

import csv
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parents[2]
CLASSIFIER = PROJECT_ROOT / "tools" / "analysis" / "classify-sculptor-sst-progress.py"
INVALID_U32 = (1 << 32) - 1

FIELDS = [
    "tile_id",
    "kind",
    "wall_time_ms",
    "simulation_tick",
    "instructions",
    "cpu_cycles",
    "task_finish_events",
    "task_finish_events_available",
    "physical_global_dma_submitted",
    "physical_global_dma_completed",
    "analog_commands_submitted",
    "analog_commands_completed",
    "local_epoch",
    "local_epoch_available",
    "memory_initialization_complete",
    "wait_reason",
    "wait_ticks",
    "network_packets",
    "network_words",
    "network_word_hops",
    "network_queue_ticks",
    "receive_dma_transfers",
    "receive_dma_words",
    "receive_dma_active_cycles",
    "pending_network_receives",
    "pending_receive_dma",
    "pending_receive_dma_descriptors",
    "pending_completed_receive_frames",
    "pending_ready_receive_bursts",
    "pending_incoming_frame_assemblies",
    "bridge_receive_bursts",
    "pending_global_dma",
    "pending_memory",
    "synchronization_events",
    "synchronization_grants",
]


def row(tile, generation, *, trace=False, bulk=False, advancing=False):
    base = {name: 0 for name in FIELDS}
    base.update(
        {
            "tile_id": tile,
            "kind": "periodic",
            "wall_time_ms": 1000 + generation * 10,
            "simulation_tick": 100 + generation * 10,
            "instructions": 20 + generation * 5,
            "cpu_cycles": 20 + generation * 5,
            "task_finish_events": generation if trace and advancing else 0,
            "task_finish_events_available": int(trace),
            "physical_global_dma_submitted": 2 + (generation if advancing else 0),
            "physical_global_dma_completed": 1 + (generation if advancing else 0),
            "analog_commands_submitted": 3 + (generation if advancing else 0),
            "analog_commands_completed": 3 + (generation if advancing else 0),
            "local_epoch": generation if bulk else INVALID_U32,
            "local_epoch_available": int(bulk),
            "memory_initialization_complete": 1,
            "wait_reason": 14,
            "pending_global_dma": 1,
            "synchronization_events": 5 + generation,
            "synchronization_grants": 2 + generation,
        }
    )
    return base


def write_evidence(
    root,
    *,
    profile_mode,
    synchronization_mode,
    advancing,
    legacy=False,
    guest_trace=None,
    work_certificate=True,
):
    (root / "trace" / "performance").mkdir(parents=True)
    (root / "uart").mkdir()
    if guest_trace is None:
        guest_trace = profile_mode == "trace"
    launch = {
        "profile_mode": profile_mode,
        "synchronization_mode": synchronization_mode,
        "guest_task_trace_enabled": guest_trace,
    }
    if work_certificate:
        launch["physical_global_dma_work"] = {
            "input_requests": 4,
            "output_requests": 2,
            "total_requests": 6,
            "tile_requests": {"0": 3, "1": 3},
        }
    (root / "launch.json").write_text(
        json.dumps(launch),
        encoding="utf-8",
    )
    (root / "launch.txt").write_text("active_tiles=2\n", encoding="utf-8")
    trace = guest_trace
    bulk = synchronization_mode == "bulk_barrier"
    fields = FIELDS
    if legacy:
        omitted = {
            "task_finish_events_available",
            "local_epoch_available",
            "memory_initialization_complete",
        }
        fields = [name for name in FIELDS if name not in omitted]
    for tile in range(2):
        path = root / "trace" / "performance" / f"tile-{tile}-progress.csv"
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for generation in range(2):
                values = row(
                    tile,
                    generation,
                    trace=trace,
                    bulk=bulk,
                    advancing=advancing and generation == 1,
                )
                writer.writerow({name: values[name] for name in fields})
        if legacy and tile == 0:
            with path.open("a", encoding="utf-8") as stream:
                stream.write("0,periodic\n")
        (root / "uart" / f"tile-{tile}.log").write_text(
            f"SCULPTOR_RA_INIT_PASS tile={tile}\n", encoding="utf-8"
        )


def run(root):
    result = subprocess.run(
        ["python3", str(CLASSIFIER), "--format", "json", str(root)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return json.loads(result.stdout)


def run_failure(root):
    return subprocess.run(
        ["python3", str(CLASSIFIER), "--format", "json", str(root)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def run_to_file(root):
    output = root / "progress-classification.json"
    subprocess.run(
        [
            "python3",
            str(CLASSIFIER),
            "--format",
            "json",
            "--output",
            str(output),
            str(root),
        ],
        check=True,
    )
    assert output.is_file()
    assert not list(root.glob(".progress-classification.json.tmp-*"))
    return json.loads(output.read_text(encoding="utf-8"))


def main():
    with tempfile.TemporaryDirectory(prefix="sst-progress-") as directory:
        root = Path(directory) / "exact"
        write_evidence(
            root,
            profile_mode="summary",
            synchronization_mode="exact_dependencies",
            advancing=True,
            legacy=True,
        )
        result = run_to_file(root)
        assert result["state"] == "active_architectural_progress"
        assert result["initialization_pass_tiles"] == 2
        assert result["task_completion"]["value"] is None
        assert result["task_completion"]["available_tiles"] == 0
        assert result["host_epoch"]["minimum"] is None
        assert result["host_epoch"]["available_tiles"] == 0
        assert result["deltas"]["physical_global_dma_completed"] == 2
        assert result["physical_global_dma_work"]["available"] is True
        assert result["physical_global_dma_work"]["completed_requests"] == 4
        assert result["physical_global_dma_work"]["remaining_requests"] == 2
        assert result["physical_global_dma_work"]["completion_percent"] == 66.666667
        assert result["deadlock_proven"] is False

        root = Path(directory) / "audit-certificate-fallback"
        write_evidence(
            root,
            profile_mode="summary",
            synchronization_mode="exact_dependencies",
            advancing=True,
            work_certificate=False,
        )
        compile_dir = Path(directory) / "compile"
        compile_dir.mkdir()
        audit_path = compile_dir / "materialization-audit.json"
        audit_path.write_text(
            json.dumps(
                {
                    "counters": {
                        "materialized_input_physical_request_count": 4,
                        "materialized_output_physical_request_count": 2,
                    },
                    "tiles": [
                        {
                            "tile_id": tile,
                            "materialized_input_physical_request_count": 2,
                            "materialized_output_physical_request_count": 1,
                        }
                        for tile in range(2)
                    ],
                }
            ),
            encoding="utf-8",
        )
        launch_path = root / "launch.json"
        launch = json.loads(launch_path.read_text(encoding="utf-8"))
        launch["source_compile_directory"] = str(compile_dir)
        launch["materialization_audit_sha256"] = hashlib.sha256(
            audit_path.read_bytes()
        ).hexdigest()
        launch_path.write_text(json.dumps(launch), encoding="utf-8")
        result = run(root)
        assert result["state"] == "active_architectural_progress"
        assert (
            result["physical_global_dma_work"]["source"]
            == "hash_bound_materialization_audit"
        )
        assert result["physical_global_dma_work"]["completed_requests"] == 4

        launch["physical_global_dma_work"] = {
            "input_requests": 3,
            "output_requests": 3,
            "total_requests": 6,
            "tile_requests": {"0": 3, "1": 3},
        }
        launch_path.write_text(json.dumps(launch), encoding="utf-8")
        failed = run_failure(root)
        assert failed.returncode != 0
        assert "disagrees with its hash-bound" in failed.stderr

        root = Path(directory) / "dma-overrun"
        write_evidence(
            root,
            profile_mode="summary",
            synchronization_mode="exact_dependencies",
            advancing=True,
        )
        launch_path = root / "launch.json"
        launch = json.loads(launch_path.read_text(encoding="utf-8"))
        launch["physical_global_dma_work"] = {
            "input_requests": 2,
            "output_requests": 2,
            "total_requests": 4,
            "tile_requests": {"0": 2, "1": 2},
        }
        launch_path.write_text(json.dumps(launch), encoding="utf-8")
        result = run(root)
        assert result["state"] == "invalid_evidence"
        assert len(result["physical_global_dma_work"]["accounting_violations"]) == 2

        root = Path(directory) / "partial-evidence"
        write_evidence(
            root,
            profile_mode="summary",
            synchronization_mode="exact_dependencies",
            advancing=True,
        )
        (root / "trace" / "performance" / "tile-1-progress.csv").unlink()
        result = run(root)
        assert result["state"] == "incomplete_evidence"
        assert result["observed_progress_tiles"] == 1
        assert result["physical_global_dma_work"]["expected_requests"] == 6

        bulk_root = Path(directory) / "bulk"
        root = bulk_root
        write_evidence(
            root,
            profile_mode="trace",
            synchronization_mode="bulk_barrier",
            advancing=False,
        )
        result = run(root)
        assert result["state"] == "executing_without_observed_architectural_progress"
        assert result["task_completion"]["value"] == 0
        assert result["host_epoch"]["minimum"] == 1
        assert result["host_epoch"]["maximum"] == 1

        root = Path(directory) / "host-trace-guest-summary"
        write_evidence(
            root,
            profile_mode="trace",
            synchronization_mode="exact_dependencies",
            advancing=True,
            legacy=True,
            guest_trace=False,
        )
        result = run(root)
        assert result["state"] == "active_architectural_progress"
        assert result["guest_task_trace_enabled"] is False
        assert result["task_completion"]["value"] is None

        root = bulk_root
        for tile in range(2):
            with (root / "uart" / f"tile-{tile}.log").open(
                "a", encoding="utf-8"
            ) as stream:
                marker = (
                    "SCULPTOR_RA_SIM_PASS"
                    if tile == 0
                    else f"SCULPTOR_RA_SIM_PASS tile={tile}"
                )
                stream.write(marker + "\n")
        result = run(root)
        assert result["state"] == "passed"
        assert result["simulation_pass_tiles"] == 2


if __name__ == "__main__":
    main()
