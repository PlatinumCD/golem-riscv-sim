"""Build and run QEMU RISC-V / banked-SPM functional and timing regressions."""
import argparse
from collections import Counter
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import build, load_build_info
from configuration import resolve


def cases():
    return [
        dict(name="default", parameters={}),
        dict(name="request-4", parameters=dict(spm_request_bytes=4)),
        dict(name="bandwidth-1", parameters=dict(spm_channel_width=1)),
        dict(name="budget-1", parameters={}, cpu_parameters=dict(instruction_budget=1)),
        dict(name="issue-4", parameters={}, cpu_parameters=dict(issue_width=4)),
        dict(name="guest-failure", parameters={}, negative=1,
             expected_failure="guest failed: exit status 7", cpu_parameters=dict(host_timeout_seconds=2)),
        # Bound the deliberately unmapped trap loop to a one-instruction grant
        # so the no-fetch guard is tested before the host watchdog can race it.
        dict(name="outside-spm", parameters={}, negative=2,
             expected_failure="no instruction-fetch progress in local SPM",
             cpu_parameters=dict(host_timeout_seconds=2, instruction_budget=1)),
    ]


def compile_guest(output, compiler, name="guest", negative=0):
    elf = output / f"{name}.elf"
    command = [str(compiler), "-march=rv64gcv_zifencei", "-mabi=lp64d", "-mcmodel=medany",
               "-msmall-data-limit=0", "-O1", "-ffreestanding", "-fno-builtin", "-nostdlib", "-static",
               "-Wl,--no-relax", "-Wl,--build-id=none", "-T", str(HERE / "scratchpad.ld"),
               str(HERE / "start.S"), str(HERE / ("negative.c" if negative else "guest.c")), "-o", str(elf)]
    if negative: command.append(f"-DNEGATIVE_KIND={negative}")
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    (output / f"{name}-build.log").write_text(process.stdout)
    process.check_returncode()
    return elf


def simulate(tools, trial, timeout):
    command = [tools["sst"], "--num-threads=1", str(HERE / "simulation.py")]
    # A log file avoids a failed child's inherited stdout pipe concealing the
    # SST exit. A process group also permits cleanup if a regression hangs.
    with (trial / "simulation.log").open("w") as log:
        process = subprocess.Popen(command,
            env=os.environ | dict(SST_LIB_PATH=tools["plugin"] + ":" + tools["library"],
                TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=timeout)
        finally:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()
    return subprocess.CompletedProcess(command, process.returncode, (trial / "simulation.log").read_text())


def report(log, label):
    matches = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(matches) == 1, (label, matches)
    return matches[0]


def validate(trial, case, log):
    p = resolve(case.get("parameters"))
    cpu, spm, peer = [report(log, label) for label in ("RISCV_STATS", "SPM_STATS", "RISCV_PEER_RESULT")]
    assert peer["passed"] and peer["verified_bytes"] == 40, peer
    assert cpu["instructions"] > 0 and cpu["vector_instructions"] >= 4, cpu
    assert cpu["read_bytes"] > 0 and cpu["write_bytes"] > 0 and cpu["fetch_bytes"] > 0, cpu
    assert cpu["memory_requests"] == cpu["completed_requests"] > 0, cpu
    assert spm["accepted"] == spm["completed"] > 0, spm
    traces = list(trial.glob("scratchpad*.csv"))
    assert len(traces) == 1, traces
    records = list(csv.DictReader(traces[0].open()))
    accepted, completed, serviced = {}, {}, Counter()
    channel_bytes = Counter()
    for row in records:
        identity, size, cycle = row["id"], int(row["bytes"]), int(row["cycle"])
        if row["event"] == "accepted":
            assert identity not in accepted
            assert 0 < size <= p["spm_request_bytes"]
            assert int(row["address"]) % p["spm_request_bytes"] + size <= p["spm_request_bytes"]
            accepted[identity] = row
        elif row["event"] == "completed":
            assert identity in accepted and identity not in completed
            assert cycle > int(accepted[identity]["cycle"])
            assert serviced[identity] == int(accepted[identity]["bytes"])
            completed[identity] = row
        else:
            assert row["event"] == "service" and identity in accepted and identity not in completed
            address = int(row["address"])
            assert address == int(accepted[identity]["address"]) + serviced[identity]
            assert int(row["bank"]) == address // p["spm_bank_width"] % p["spm_banks"]
            serviced[identity] += size
            key = cycle, row["pool"], row["channel"]
            channel_bytes[key] += size
            assert channel_bytes[key] <= p["spm_channel_width"]
    assert len(accepted) == len(completed) == spm["accepted"]
    cpu_requests = [r for r in accepted.values() if r["requestor"].endswith(":qemu_memory")]
    peer_requests = [r for r in accepted.values() if r["requestor"].startswith("peer")]
    assert cpu_requests and peer_requests, {r["requestor"] for r in accepted.values()}
    assert len(cpu_requests) == cpu["memory_requests"]
    assert sum(int(r["bytes"]) for r in cpu_requests if r["write"] == "0") == cpu["read_bytes"] + cpu["fetch_bytes"]
    assert sum(int(r["bytes"]) for r in cpu_requests if r["write"] == "1") == cpu["write_bytes"]
    for write in ("0", "1"):
        assert sum(int(r["bytes"]) for r in accepted.values() if r["write"] == write) == spm["write_bytes" if write == "1" else "read_bytes"]
    # These are the actual scalar/RVV result bytes, read by an independent SPM
    # client. Their successful verification does not depend on QEMU's own loads.
    output_writes = [r for r in cpu_requests if r["write"] == "1" and 0x100044 <= int(r["address"]) < 0x100064]
    assert sum(int(r["bytes"]) for r in output_writes) == 32, output_writes
    ready_writes = [r for r in cpu_requests if r["write"] == "1" and int(r["address"]) == 0x100040]
    assert len(ready_writes) == 1, ready_writes
    ready_completed = int(completed[ready_writes[0]["id"]]["cycle"])
    poll_requests = [r for r in peer_requests if r["write"] == "0" and int(r["address"]) == 0x100040]
    polls = {int(r["index"]): r for r in csv.DictReader((trial / "peer-polls.csv").open())}
    assert len(poll_requests) == len(polls)
    checked_polls = 0
    for index, request in enumerate(poll_requests):
        if int(request["cycle"]) >= ready_completed:
            assert int(polls[index]["value"]) == 0xc001c0de, (
                "peer read following CPU write saw stale shared memory", request, polls[index])
            checked_polls += 1
    assert checked_polls >= 1, "same-line ordering regression did not exercise a post-write read"
    return dict(RISCV_STATS=cpu, SPM_STATS=spm, RISCV_PEER_RESULT=peer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", help="Select a named regression")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/riscv-gnu-toolchain/bin/riscv64-unknown-elf-gcc")
    parser.add_argument("--output", type=Path, help="Keep build products and reports in this directory")
    parser.add_argument("--build-info", type=Path, help="Reuse a verified component build")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {c["name"] for c in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [c for c in selected if c["name"] in args.case]
    qemu = args.qemu.resolve()
    if not qemu.is_file():
        parser.error(f"QEMU missing: {qemu}; run source_new/components/riscv-qemu/build_qemu.py first")
    output = (args.output or ROOT / "tests/results/source-new-riscv" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    elf = compile_guest(output, args.compiler.resolve())
    negative_elves = {case["negative"]: compile_guest(output, args.compiler.resolve(), case["name"], case["negative"])
                      for case in selected if case.get("negative")}
    tools = load_build_info(args.build_info, extra_sources=[HERE / "peer.cc"]) if args.build_info else build(output / "build", extra_sources=[HERE / "peer.cc"])
    results = []
    for case in selected:
        trial = output / case["name"]
        trial.mkdir()
        case = case | dict(elf=str(negative_elves.get(case.get("negative"), elf)), qemu=str(qemu))
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        (trial / "parameters.json").write_text(json.dumps(resolve(case.get("parameters")), indent=2) + "\n")
        start = time.perf_counter()
        process = simulate(tools, trial, 15 if case.get("expected_failure") else 120)
        if case.get("expected_failure"):
            assert process.returncode != 0 and case["expected_failure"] in process.stdout, process.stdout
            verified = dict(EXPECTED_FAILURE=dict(passed=True, diagnostic=case["expected_failure"]))
        elif process.returncode:
            print(process.stdout[-8000:]); process.check_returncode()
        else:
            verified = validate(trial, case, process.stdout)
        results.append(dict(case=case["name"], host_wall_seconds=time.perf_counter() - start, **verified))
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        if case.get("expected_failure"):
            print(f"PASS {case['name']}: rejected with {case['expected_failure']}", flush=True)
        else:
            print(f"PASS {case['name']}: {verified['RISCV_STATS']['end_cycle']} cycles, "
                  f"{verified['RISCV_STATS']['memory_requests']} SPM requests", flush=True)
    by_case = {r["case"]: r for r in results}
    if {"default", "bandwidth-1"} <= by_case.keys():
        assert by_case["bandwidth-1"]["RISCV_STATS"]["end_cycle"] > by_case["default"]["RISCV_STATS"]["end_cycle"], "SPM bandwidth must affect CPU runtime"
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS: {len(results)} RISC-V / banked-SPM regressions", flush=True)


if __name__ == "__main__": main()
