"""Exercise instruction-cache hits, refills, conflicts, and fence.i via QEMU/SST."""
import argparse
from collections import Counter
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from build import build
from configuration import resolve

CACHE_KEYS = tuple("icache_" + name for name in (
    "fetches", "hits", "misses", "fills", "fill_bytes", "evictions", "invalidations", "stall_cycles"))


def cases():
    return [dict(name="default"),
        dict(name="tiny", cpu_parameters=dict(instruction_cache_bytes=128)),
        dict(name="uncached", cpu_parameters=dict(instruction_cache_enabled=False)),
        dict(name="hit-4", cpu_parameters=dict(instruction_cache_hit_cycles=4)),
        dict(name="budget-1", cpu_parameters=dict(instruction_budget=1)),
        dict(name="budget-7", cpu_parameters=dict(instruction_budget=7)),
        dict(name="issue-4", cpu_parameters=dict(issue_width=4)),
        dict(name="line-4", cpu_parameters=dict(instruction_cache_bytes=64, instruction_cache_line_bytes=4)),
        dict(name="request-4", parameters=dict(spm_request_bytes=4)),
        dict(name="bandwidth-1", parameters=dict(spm_channel_width=1))]


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_policy(output):
    component = SOURCE / "components/riscv-qemu"
    executable = output / "instruction-cache-policy"
    command = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(component), str(HERE / "policy.cc"),
        str(component / "instructionCache.cc"), "-o", str(executable)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (output / "policy-build.log").write_text(process.stdout + process.stderr)
    if process.returncode:
        raise RuntimeError(f"Native policy build failed: {process.stderr}")
    process = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    (output / "policy.log").write_text(process.stdout + process.stderr)
    if process.returncode:
        raise RuntimeError(f"Native policy checks failed: {process.stderr}")
    assert "PASS instruction-cache native policy" in process.stdout


def compile_guest(output, compiler):
    elf = output / "guest.elf"
    support = HERE.parent / "riscv-qemu"
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog",
        "-fuse-ld=lld", "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0",
        "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none",
        "-T", str(support / "scratchpad.ld"), str(support / "start.S"),
        str(HERE / "measurement.S"), str(HERE / "loops.S"), str(HERE / "guest.c"), "-o", str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    (output / "guest-build.log").write_text(process.stdout + process.stderr)
    if process.returncode:
        raise RuntimeError(f"Guest build failed: {process.stderr}")
    assembly = subprocess.check_output([str(compiler.parent / "llvm-objdump"), "-d", str(elf)], text=True)
    (output / "guest.asm").write_text(assembly)
    assert "fence.i" in assembly
    names = subprocess.check_output([str(compiler.parent / "llvm-nm"), "-n", str(elf)], text=True)
    symbols = {fields[2]: int(fields[0], 16) for line in names.splitlines()
               if len(fields := line.split()) == 3}
    assert symbols["boundary_instruction"] % 64 == 62
    assert symbols["small_boundary_instruction"] % 64 == 2
    assert all(symbols[name] % 64 == 0 for name in ("hot_loop", "conflict_a", "conflict_b", "conflict_c", "modified_code"))
    assert len({symbols[name] // 64 for name in ("conflict_a", "conflict_b", "conflict_c")}) == 3
    return dict(elf=str(elf), sha256=sha256(elf), command=command, symbols=symbols)


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, label):
    found = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(found) == 1, (label, found)
    return found[0]


def validate(trial, case, guest, log):
    cpu, spm = stats(log, "RISCV_STATS"), stats(log, "SPM_STATS")
    p = case["parameters"]
    cached = case["cpu_parameters"].get("instruction_cache_enabled", True)
    line_bytes = case["cpu_parameters"].get("instruction_cache_line_bytes", 64)
    assert cpu["memory_requests"] == cpu["completed_requests"] == spm["accepted"] == spm["completed"] > 0
    assert cpu["analog_commands"] == 0
    with (trial / "scratchpad.bin").open("rb") as memory:
        memory.seek(0x100000)
        assert struct.unpack("<6I", memory.read(24)) == (384, 192, 37, 17, 29, 41)
    if cached:
        assert cpu["icache_fetches"] == cpu["icache_hits"] + cpu["icache_misses"] > 0
        assert cpu["icache_hits"] > cpu["icache_misses"] > 0
        assert cpu["fetch_bytes"] == cpu["icache_fill_bytes"] == cpu["icache_fills"] * line_bytes
        assert cpu["icache_invalidations"] == 1
        assert cpu["icache_stall_cycles"] > 0
    else:
        assert all(cpu[key] == 0 for key in CACHE_KEYS)
        assert cpu["fetch_bytes"] == cpu["instruction_bytes"] > 0

    markers = rows(trial / "riscv-tasks.csv")
    assert len(markers) == 8
    phases = {}
    for index in range(4):
        start, finish = markers[index * 2:index * 2 + 2]
        assert start["event"] == "start" and finish["event"] == "finish"
        assert int(start["task_id"]) == int(finish["task_id"]) == index + 1
        assert int(start["execution_id"]) == int(finish["execution_id"]) == 0
        assert start["memory_requests"] == start["completed_requests"]
        assert finish["memory_requests"] == finish["completed_requests"]
        phase = {key: int(finish[key]) - int(start[key]) for key in start
                 if key not in ("event", "task_id", "execution_id")}
        phase.update(start_cycle=int(start["cycle"]), end_cycle=int(finish["cycle"]))
        assert phase["cycle"] > 0
        phases[str(index + 1)] = phase
    if cached:
        assert phases["1"]["icache_hits"] >= 380
        if line_bytes == 64:
            assert phases["1"]["icache_misses"] <= 8
        assert phases["4"]["icache_invalidations"] == 1
        if case["name"] == "tiny":
            assert phases["2"]["icache_misses"] >= 96
            assert cpu["icache_evictions"] > 0

    # SPM byte accounting and bank/channel scheduling independently check that
    # fills really traverse the shared backend, including four-byte splitting.
    accepted, completed, served, channels = {}, set(), Counter(), Counter()
    traces = list(trial.glob("scratchpad*.csv"))
    assert len(traces) == 1, traces
    for row in rows(traces[0]):
        identity, count, cycle = row["id"], int(row["bytes"]), int(row["cycle"])
        address = int(row["address"])
        if row["event"] == "accepted":
            assert identity not in accepted
            assert 0 < count <= p["spm_request_bytes"]
            assert address % p["spm_request_bytes"] + count <= p["spm_request_bytes"]
            accepted[identity] = row
        elif row["event"] == "completed":
            assert identity in accepted and identity not in completed
            assert served[identity] == int(accepted[identity]["bytes"])
            completed.add(identity)
        else:
            assert row["event"] == "service" and identity in accepted and identity not in completed
            assert address == int(accepted[identity]["address"]) + served[identity]
            assert int(row["bank"]) == address // p["spm_bank_width"] % p["spm_banks"]
            served[identity] += count
            channels[cycle, row["pool"], row["channel"]] += count
            assert channels[cycle, row["pool"], row["channel"]] <= p["spm_channel_width"]
    assert len(accepted) == len(completed) == cpu["memory_requests"]
    assert sum(int(row["bytes"]) for row in accepted.values() if row["write"] == "0") == cpu["read_bytes"] + cpu["fetch_bytes"] == spm["read_bytes"]
    assert sum(int(row["bytes"]) for row in accepted.values() if row["write"] == "1") == cpu["write_bytes"] == spm["write_bytes"]

    cache_trace = rows(trial / "riscv-icache.csv")
    if not cached:
        assert not cache_trace
    else:
        events = Counter(row["event"] for row in cache_trace)
        for event, counter in (("hit", "hits"), ("miss", "misses"), ("fill", "fills"), ("invalidate", "invalidations")):
            assert events[event] == cpu["icache_" + counter], (event, events, cpu)
        fills = [row for row in cache_trace if row["event"] == "fill"]
        assert all(int(row["address"]) % line_bytes == 0 and int(row["bytes"]) == line_bytes for row in fills)
        boundary = guest["symbols"]["boundary_instruction"]
        phase = phases["3"]
        assert any(row["event"] == "miss" and int(row["address"]) == boundary and int(row["bytes"]) == 4 for row in cache_trace)
        boundary_fills = {int(row["address"]) for row in fills
                          if phase["start_cycle"] <= int(row["cycle"]) <= phase["end_cycle"]}
        assert {boundary - boundary % line_bytes, boundary + 2} <= boundary_fills, boundary_fills
        if line_bytes == 4:
            small = guest["symbols"]["small_boundary_instruction"]
            assert (small - 2) // p["spm_request_bytes"] == (small + 2) // p["spm_request_bytes"]
            assert {small - 2, small + 2} <= boundary_fills, boundary_fills
        invalidation = next(int(row["cycle"]) for row in cache_trace if row["event"] == "invalidate")
        modified = guest["symbols"]["modified_code"]
        assert any(int(row["address"]) == modified and int(row["cycle"]) < invalidation for row in fills)
        assert any(int(row["address"]) == modified and int(row["cycle"]) > invalidation for row in fills)
    return dict(case=case["name"], cpu=cpu, spm=spm, phases=phases, outputs_checked=6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--build-info", type=Path, help="Reuse a component after checking recorded C++ source/header hashes")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case["name"] for case in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [case for case in selected if case["name"] in args.case]
    output = (args.output or ROOT / "tests/results/source-new-instruction-cache" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    check_policy(output)
    guest = compile_guest(output, args.compiler.resolve())
    if args.build_info:
        tools = json.loads(args.build_info.read_text())
        for path, digest in tools["source_sha256"].items():
            if Path(path).suffix in (".h", ".cc"):
                assert sha256(path) == digest, f"C++ source changed: {path}; rebuild"
    else:
        tools = build(output / "build")
    (output / "metadata.json").write_text(json.dumps(dict(guest=guest, build=tools,
        qemu=str(args.qemu.resolve()), qemu_sha256=sha256(args.qemu.resolve())), indent=2) + "\n")
    results = []
    for entry in selected:
        trial = output / entry["name"]
        trial.mkdir()
        case = entry | dict(parameters=resolve(entry.get("parameters")),
            cpu_parameters=entry.get("cpu_parameters", {}), elf=guest["elf"], qemu=str(args.qemu.resolve()))
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        start = time.perf_counter()
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen([tools["sst"], "--num-threads=1", str(HERE / "simulation.py")],
                env=os.environ | dict(SST_LIB_PATH=tools["plugin"] + ":" + tools["library"],
                    TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try: process.wait(timeout=120)
            finally:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.wait()
        log = (trial / "simulation.log").read_text()
        if process.returncode:
            raise RuntimeError(f"{case['name']} failed: {log[-8000:]}")
        result = validate(trial, case, guest, log)
        result["host_wall_seconds"] = time.perf_counter() - start
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: {result['cpu']['end_cycle']} cycles", flush=True)
    by_case = {result["case"]: result for result in results}
    if "default" in by_case:
        reference = by_case["default"]
        for result in results:
            assert result["cpu"]["instructions"] == reference["cpu"]["instructions"]
            assert result["cpu"]["instruction_bytes"] == reference["cpu"]["instruction_bytes"]
        if "tiny" in by_case:
            assert by_case["tiny"]["phases"]["2"]["icache_misses"] > reference["phases"]["2"]["icache_misses"]
        if "uncached" in by_case:
            assert by_case["uncached"]["cpu"]["fetch_bytes"] > reference["cpu"]["fetch_bytes"]
            assert by_case["uncached"]["cpu"]["end_cycle"] > reference["cpu"]["end_cycle"]
        if "hit-4" in by_case:
            assert by_case["hit-4"]["cpu"]["end_cycle"] - reference["cpu"]["end_cycle"] == 3 * reference["cpu"]["icache_fetches"]
        for name in ("budget-1", "budget-7"):
            if name in by_case:
                assert by_case[name]["cpu"]["end_cycle"] == reference["cpu"]["end_cycle"]
                assert by_case[name]["cpu"]["issue_cycles"] == reference["cpu"]["issue_cycles"]
        if "issue-4" in by_case:
            assert by_case["issue-4"]["cpu"]["end_cycle"] == reference["cpu"]["end_cycle"]
        if "request-4" in by_case:
            assert by_case["request-4"]["cpu"]["fetch_bytes"] == reference["cpu"]["fetch_bytes"]
            assert by_case["request-4"]["cpu"]["memory_requests"] > reference["cpu"]["memory_requests"]
        if "bandwidth-1" in by_case:
            assert by_case["bandwidth-1"]["cpu"]["end_cycle"] > reference["cpu"]["end_cycle"]
    (output / "validation.json").write_text(json.dumps(dict(passed=True, native_policy=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS {len(results)} instruction-cache cases", flush=True)


if __name__ == "__main__": main()
