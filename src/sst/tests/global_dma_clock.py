#!/usr/bin/env python3
"""Integrated global-DMA clock oracle; never builds or installs SST/QEMU.

Reports legacy failures as failures, not compatibility expectations. Exit 1
means a required tree violates the oracle; exit 2 means invalid evidence.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CPU_TICKS = {"500MHz": 2000, "1GHz": 1000, "2GHz": 500}
RAM_TICKS = 1000
LINK_TICKS = 1000
TRANSFER_BYTES = 128
DMA_BYTES_PER_CPU_CYCLE = 32


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def execute(command, env, directory, timeout):
    with (directory / "simulation.log").open("w") as log:
        process = subprocess.Popen(command, cwd=directory, env=env,
                                   stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            return "TIMEOUT"


def analyze(trial, mode, cpu, setup):
    serial = (trial / "serial/tile-0.log").read_text()
    assert "GLOBAL_DMA_CLOCK_PAYLOAD_PASS" in serial, "guest payload check failed"
    tasks = rows(trial / "tasks/tile-0.csv")
    assert len(tasks) == 2 and [r["event"] for r in tasks] == ["start", "finish"], tasks
    start, finish = (int(r["sim_time_ticks"]) for r in tasks)
    waits = [r for r in rows(trial / "profile/tile-0-waits.csv")
             if int(r["start_tick"]) >= start and int(r["finish_tick"]) <= finish]
    submits = [r for r in waits if r["reason"] == "scratchpad-dma-submit"]
    expected_reason = "scratchpad-dma-wait" + ("-batch" if mode == "batch" else "")
    retirements = [r for r in waits if r["reason"] == expected_reason]
    assert len(submits) == 2, submits
    assert len(retirements) == (1 if mode == "batch" else 2), retirements
    assert all(int(r["duration_ticks"]) == 0 for r in submits), submits
    requests = [r for r in rows(trial / "profile/global-ram-requests.csv")
                if int(r["execution_id"]) == 501]
    assert len(requests) == 2 and [int(r["token_id"]) for r in requests] == [0, 1], requests
    assert all(r["direction"] == "write" and int(r["byte_count"]) == TRANSFER_BYTES
               for r in requests), requests

    # Idle banks, one GlobalRAM DMA frontend, latency=1 CPU cycle, four
    # consecutive 32-byte beats: each reservation costs setup+4 CPU cycles.
    # No CPU SPM accesses or DMA waits occur between these two submissions.
    service_cpu_cycles = setup + TRANSFER_BYTES // DMA_BYTES_PER_CPU_CYCLE
    cpu_ticks = CPU_TICKS[cpu]
    previous_due_tick = 0
    tokens = []
    for token, (submit, request) in enumerate(zip(submits, requests)):
        submit_tick = int(submit["start_tick"])
        due_tick = max(submit_tick, previous_due_tick) + service_cpu_cycles * cpu_ticks
        previous_due_tick = due_tick
        # Controller cycles are in its own 1GHz domain. Include one RAM cycle
        # of possible phase rounding when proving the RAM response is earlier.
        ram_reply_upper_tick = (int(request["completion_cycle"]) + 1) * RAM_TICKS + LINK_TICKS
        assert ram_reply_upper_tick < due_tick, "RAM, not local SPM, dominates"
        tokens.append({"token": token, "submit_tick": submit_tick,
                       "spm_service_cpu_cycles": service_cpu_cycles,
                       "correct_spm_deadline_tick": due_tick,
                       "ram_arrival_cycle": int(request["arrival_cycle"]),
                       "ram_completion_cycle": int(request["completion_cycle"]),
                       "ram_reply_upper_tick": ram_reply_upper_tick})

    observations = []
    for index, wait in enumerate(retirements):
        token = 1 if mode == "batch" else index
        due_tick = tokens[token]["correct_spm_deadline_tick"]
        wait_start_tick = int(wait["start_tick"])
        actual_tick = int(wait["finish_tick"])
        # The completion cannot predate either the reservation deadline or the
        # wait itself. At most one CPU period of rounding is permitted.
        earliest_tick = max(due_tick, wait_start_tick)
        latest_tick = earliest_tick + cpu_ticks - 1
        status = ("EARLY_RETIREMENT" if actual_tick < earliest_tick else
                  "OVERSCHEDULED" if actual_tick > latest_tick else "PASS")
        observations.append({"token": token, "wait_start_tick": wait_start_tick,
                             "actual_retirement_tick": actual_tick,
                             "earliest_legal_retirement_tick": earliest_tick,
                             "latest_legal_retirement_tick": latest_tick,
                             "error_ticks": actual_tick - earliest_tick,
                             "status": status})

    # Diagnostic only, never the correctness oracle: recognize the independent
    # latch bug from existing event evidence. The first response arms the local
    # timer; the second response must not retire that first wait before either
    # the correct deadline or even the legacy (mis-unitized) deadline.
    premature_other_completion = False
    if mode == "scalar":
        first_reply_tick = int(requests[0]["completion_cycle"]) * RAM_TICKS + LINK_TICKS
        other_reply_tick = int(requests[1]["completion_cycle"]) * RAM_TICKS + LINK_TICKS
        legacy_deadline_tick = tokens[0]["submit_tick"] + service_cpu_cycles
        first = observations[0]
        premature_other_completion = (
            first["wait_start_tick"] < first_reply_tick < other_reply_tick < legacy_deadline_tick
            and first["actual_retirement_tick"] == other_reply_tick
            and first["status"] == "EARLY_RETIREMENT")
    return {"oracle_status": "PASS" if all(o["status"] == "PASS" for o in observations) else "FAIL",
            "payload_status": "PASS", "cpu_ticks_per_cycle": cpu_ticks,
            "ram_ticks_per_cycle": RAM_TICKS, "tick_seconds": "1e-12",
            "region_start_tick": start, "region_finish_tick": finish,
            "tokens": tokens, "waits": observations,
            "premature_other_completion_observed": premature_other_completion}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", action="append", choices=("src", "reference"))
    parser.add_argument('--reference-install', type=Path, help='Explicit historical hardware installation')
    parser.add_argument("--cpu", action="append", choices=tuple(CPU_TICKS))
    parser.add_argument("--setup", action="append", type=int,
                        help="SPM DMA setup CPU cycles (default: 10000 and 1000000)")
    parser.add_argument("--no-interrupt", action="store_true")
    parser.add_argument("--timeout", type=int, default=20)
    parser.add_argument("--require-tree", action="append", choices=("src", "reference"),
                        help="Trees gating exit status; defaults to current src when selected")
    args = parser.parse_args()
    trees = args.tree or ['src']
    required = args.require_tree or (['src'] if 'src' in trees else trees)
    if 'reference' in trees and (args.reference_install is None or not args.reference_install.is_dir()):
        parser.error('--tree reference requires an existing --reference-install')
    if not set(required) <= set(trees):
        parser.error("required trees must be selected")
    setups = args.setup or [10000, 1000000]
    if any(s < 10000 for s in setups):
        parser.error("setup must be at least 10000 CPU cycles to isolate local SPM")
    evidence_root = Path(os.environ.get('GOLEM_BUILD_ROOT', ROOT / 'build/src'))
    evidence_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="global-dma-clocks-", dir=evidence_root))
    print(f"Global DMA clock evidence: {output}", flush=True)
    base_env = {k: v for k, v in os.environ.items()
                if not k.startswith(("MITTENS_", "GLOBAL_DMA_CLOCK_"))}
    guest = output / "guest"
    guest.mkdir()
    build_command = ["bash", str(HERE / "global_dma_clock_build.sh"), str(guest)]
    build_env = dict(base_env, GOLEM_HARDWARE_TREE="src",
                     GOLEM_BUILD_ROOT=str(output),
                     GOLEM_INSTALL_ROOT=os.environ.get('GOLEM_INSTALL_ROOT', str(ROOT / 'install/src')))
    with (output / "guest-build.log").open("w") as log:
        subprocess.run(build_command, cwd=ROOT, env=build_env, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=30)

    binaries = {}
    for tree in trees:
        install = (args.reference_install.resolve() if tree == 'reference' else
                   Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install/src')))
        binaries[tree] = {
            "qemu": install / "qemu/bin/qemu-system-riscv64",
            "element": install / "sst-elements/lib/sst-elements-library/libmittens.so",
        }
    before = {tree: {key: digest(path) for key, path in files.items()}
              for tree, files in binaries.items()}
    cases = [(cpu, mode, setup, 0)
             for cpu in (args.cpu or list(CPU_TICKS))
             for mode in ("scalar", "batch") for setup in setups]
    if not args.no_interrupt:
        cases.append(("1GHz", "scalar", 1000000, 100))
    report = {"output": str(output), "build_command": build_command,
              "required_trees": required, "binary_sha256_before": before,
              "binary_paths": {t: {k: str(p) for k, p in b.items()} for t, b in binaries.items()},
              "guest_sha256": {m: digest(guest / f"{m}.elf") for m in ("scalar", "batch")},
              "fixture_sha256": {p.name: digest(p) for p in HERE.glob("global_dma_clock*") if p.is_file()},
              "cases": []}
    for tree in trees:
        for cpu, mode, setup, ram_latency in cases:
            name = f"{cpu}-{mode}-spm{setup}-ram{ram_latency}"
            trial = output / tree / name
            for directory in ("profile", "serial", "tasks"):
                (trial / directory).mkdir(parents=True, exist_ok=True)
            env = dict(base_env,
                       SST_LIB_PATH=str(binaries[tree]["element"].parent),
                       GLOBAL_DMA_CLOCK_TRIAL=str(trial),
                       GLOBAL_DMA_CLOCK_QEMU=str(binaries[tree]["qemu"]),
                       GLOBAL_DMA_CLOCK_ELF=str(guest / f"{mode}.elf"),
                       GLOBAL_DMA_CLOCK_CPU=cpu, GLOBAL_DMA_CLOCK_SPM_SETUP=str(setup),
                       GLOBAL_DMA_CLOCK_RAM_LATENCY=str(ram_latency),
                       GOLEM_RESOLVED_CONFIG_DIR=str(trial / "resolved"))
            command = [str(ROOT / "install/sst-core/bin/sst"),
                       str(HERE / "global_dma_clock_simulation.py")]
            started = time.monotonic()
            code = execute(command, env, trial, args.timeout)
            result = {"tree": tree, "case": name, "cpu_clock": cpu, "mode": mode,
                      "setup_cpu_cycles": setup, "ram_fixed_latency_cycles": ram_latency,
                      "trial": str(trial), "command": command, "exit_code": code,
                      "wall_seconds": time.monotonic() - started}
            try:
                assert code == 0, f"simulation exit: {code}"
                result.update(analyze(trial, mode, cpu, setup))
            except (AssertionError, OSError, ValueError, KeyError) as error:
                result.update(oracle_status="INVALID_EVIDENCE", error=str(error))
            report["cases"].append(result)
            save(trial / "oracle.json", result)
            save(output / "results.json", report)
            print(f"{tree} {name}: {result['oracle_status']} "
                  f"({result['wall_seconds']:.2f}s)", flush=True)
    after = {tree: {key: digest(path) for key, path in files.items()}
             for tree, files in binaries.items()}
    report["binary_sha256_after"] = after
    report["binaries_unchanged"] = before == after
    invalid = before != after or any(c["oracle_status"] == "INVALID_EVIDENCE" for c in report["cases"])
    failed = any(c["tree"] in required and c["oracle_status"] != "PASS" for c in report["cases"])
    report["status"] = "INVALID_EVIDENCE" if invalid else "FAIL" if failed else "PASS"
    save(output / "results.json", report)
    print(f"{report['status']}: {output / 'results.json'}", flush=True)
    return 2 if invalid else 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
