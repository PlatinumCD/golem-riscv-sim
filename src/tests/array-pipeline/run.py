"""Check array pipeline snapshots, FIFO retirement, overlap, and backpressure."""
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


def cases():
    result = []
    for dimension in (32, 64):
        for vlen in (256, 512):
            for enabled in (False, True):
                result.append(dict(name=f"array{dimension}-vlen{vlen}-{'enabled' if enabled else 'disabled'}",
                    dimension=dimension, vlen=vlen, enabled=enabled, cost=4000))
    result += [dict(name="default-enabled", dimension=32, vlen=256, enabled=True, cost=4000,
                   use_pipeline_default=True),
               dict(name="budget-1", dimension=32, vlen=256, enabled=True, cost=4000,
                   cpu_parameters=dict(instruction_budget=1)),
               dict(name="zero-cost", dimension=32, vlen=256, enabled=True, cost=0),
               dict(name="exit-pending", dimension=32, vlen=256, enabled=True, cost=4000, exit_pending=True),
               dict(name="fence-i-pending", dimension=32, vlen=256, enabled=True, cost=4000, exit_pending=True, drain_kind=1),
               dict(name="marker-pending", dimension=32, vlen=256, enabled=True, cost=4000, exit_pending=True, drain_kind=2),
               dict(name="protocol", dimension=2, vlen=256, enabled=True, cost=4000, protocol=True),
               dict(name="protocol-zero", dimension=2, vlen=256, enabled=True, cost=0, protocol=True)]
    return result


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compile_guest(output, compiler, dimension, enabled, exit_pending, drain_kind):
    elf = output / f"guest-{dimension}-{int(enabled)}-{int(exit_pending)}-{drain_kind}.elf"
    support = HERE.parent / "riscv-qemu"
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog",
        "-fuse-ld=lld", "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0",
        "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none",
        f"-DARRAY_DIM={dimension}", f"-DPIPELINE={int(enabled)}", f"-DEXIT_PENDING={int(exit_pending)}",
        f"-DDRAIN_KIND={drain_kind}",
        "-T", str(support / "scratchpad.ld"), str(support / "start.S"),
        str(HERE.parent / "vector-analog/trap.S"), str(HERE / "measurement.S"), str(HERE / "guest.c"),
        "-o", str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    elf.with_suffix(".build.log").write_text(process.stdout + process.stderr)
    if process.returncode: raise RuntimeError(process.stderr)
    return dict(elf=str(elf), sha256=sha256(elf), command=command)


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, label):
    result = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(result) == 1, (label, result)
    return result[0]


def validate(trial, case, log):
    p = case["parameters"]
    arrays = stats(log, "ARRAY_STATS")
    topology = json.loads((trial / "topology.json").read_text())
    array, = (node for node in topology["components"] if node["type"] == "tilecomponents.AnalogArrays")
    assert int(array["params"]["arrays_per_tile"]) == 1
    assert str(array["params"]["array_pipeline_enabled"]).lower() in (("true", "1") if case["enabled"] else ("false", "0"))
    assert not array.get("subcomponents") and not any(key.startswith("spm_") for key in array["params"])
    array_links = []
    for link in topology["links"]:
        for end in ("left", "right"):
            if link[end]["component"] == array["name"]:
                assert link[end]["port"] == "commands"
                array_links.append(link)
    assert len(array_links) == 1

    trace = rows(trial / "arrays.csv")
    starts, commands, bandwidth = {}, [], Counter()
    for row in trace:
        if row["event"] == "start":
            assert row["token"] not in starts
            starts[row["token"]] = row
        elif row["event"] == "complete":
            start = starts.pop(row["token"])
            commands.append(dict(token=int(row["token"]), operation=int(row["operation"]),
                count=int(row["element_count"]), begin=int(start["cycle"]), end=int(row["cycle"])))
        elif row["event"] == "error": starts.pop(row["token"], None)
        elif row["event"] in ("link_read", "link_write"):
            bandwidth[int(row["cycle"])] += int(row["bytes"])
    assert not starts and len(commands) == arrays["completed"]
    assert max(bandwidth.values()) <= p["array_link_width"]
    computes = [command for command in commands if command["operation"] == 2]
    assert all(command["end"] - command["begin"] == case["cost"] for command in computes)
    computes.sort(key=lambda command: command["begin"])
    assert all(left["end"] <= right["begin"] for left, right in zip(computes, computes[1:])), "one compute unit overlapped itself"
    assert arrays["busy"] == 0

    if case.get("protocol"):
        report = stats(log, "PIPELINE_PROTOCOL_RESULT")
        assert report["passed"] and report["commands"] == arrays["completed"] == 12
        assert arrays["errors"] == 0 and arrays["mvms"] == 3
        assert report["third_execute_backpressured"] and report["duplicate_coverage_preserved"]
        by_token = {command["token"]: command for command in commands}
        assert by_token[7]["begin"] >= by_token[10]["end"], "full FIFO admitted third execution early"
        return dict(case=case["name"], arrays=arrays, protocol=report)

    cpu, spm = stats(log, "RISCV_STATS"), stats(log, "SPM_STATS")
    assert cpu["memory_requests"] == cpu["completed_requests"] == spm["accepted"] == spm["completed"]
    assert cpu["analog_commands"] == arrays["completed"]
    assert cpu["analog_read_bytes"] == arrays["link_write_bytes"]
    assert cpu["analog_write_bytes"] == arrays["link_read_bytes"]
    requests = rows(next(trial.glob("scratchpad*.csv")))
    accepted = [row for row in requests if row["event"] == "accepted"]
    assert all(row["requestor"].endswith(":qemu_memory") for row in accepted)
    assert len(accepted) == cpu["memory_requests"]
    dimension = case["dimension"]
    jobs = 1 if case.get("exit_pending") else 5
    assert arrays["mvms"] == len(computes) == jobs
    assert arrays["errors"] == (2 if not case["enabled"] or case.get("exit_pending") else 3)
    assert arrays["link_read_bytes"] == 4 * (dimension * dimension + 2 + jobs * dimension)
    if case.get("exit_pending"):
        assert arrays["link_write_bytes"] == 0 and cpu["end_cycle"] >= computes[0]["end"]
        if case.get("drain_kind") == 1:
            invalidations = [row for row in rows(trial / "riscv-icache.csv") if row["event"] == "invalidate"]
            assert len(invalidations) == 1 and int(invalidations[0]["cycle"]) >= computes[0]["end"]
        if case.get("drain_kind") == 2:
            markers = rows(trial / "riscv-tasks.csv")
            assert [row["event"] for row in markers] == ["start", "finish"]
            assert int(markers[0]["cycle"]) >= computes[0]["end"]
            assert int(markers[0]["analog_commands"]) == cpu["analog_commands"]
        return dict(case=case["name"], cpu=cpu, arrays=arrays, outputs_checked=0)
    assert arrays["link_write_bytes"] == 5 * (dimension + 3) * 4
    oracle = [[sum(((r * 3 + c * 5) % 11 - 5) * ((c * 3 + job * 5) % 13 - 6)
                   for c in range(dimension)) for r in range(dimension)] for job in range(5)]
    with (trial / "scratchpad.bin").open("rb") as memory:
        memory.seek(0x100000)
        output = struct.unpack(f"<{5 * dimension}f", memory.read(5 * dimension * 4))
        assert list(output) == [value for job in oracle for value in job]
        memory.seek(0x110000)
        tail_bytes = memory.read(5 * 16 * 4)
        memory.seek(0x120000)
        maximum, traps = struct.unpack("<2I", memory.read(8))
        assert maximum == case["vlen"] // 32 and traps == int(case["enabled"])
    for job in range(5):
        actual = struct.unpack_from("<3f", tail_bytes, job * 64)
        assert list(actual) == oracle[job][-3:]
        assert all(value == 0xffffffff for value in struct.unpack_from(f"<{maximum - 3}I", tail_bytes, job * 64 + 12))
    markers = rows(trial / "riscv-tasks.csv")
    assert len(markers) == 2 and [row["event"] for row in markers] == ["start", "finish"]
    begin, end = (int(row["cycle"]) for row in markers)
    assert begin < computes[0]["begin"] <= computes[-1]["end"] <= end

    overlapping = {}
    for operation, label in ((1, "load"), (3, "store")):
        overlapping[label] = sum(any(compute["begin"] < transfer["end"] and transfer["begin"] < compute["end"]
                                     for compute in computes)
                                 for transfer in commands if transfer["operation"] == operation and transfer["count"])
    overlapping["spm_writes"] = sum(any(compute["begin"] < int(row["cycle"]) < compute["end"] for compute in computes)
        for row in requests if row["event"] == "service" and row["write"] == "1")
    if case["enabled"] and case["cost"]:
        assert overlapping["load"] > 0 and overlapping["store"] > 0 and overlapping["spm_writes"] > 0, overlapping
        assert arrays["compute_load_overlap_cycles"] > 0 and arrays["compute_store_overlap_cycles"] > 0
        assert arrays["peak_pending_jobs_per_array"] == 2
    elif not case["enabled"]:
        assert overlapping["load"] == overlapping["store"] == 0
    return dict(case=case["name"], cpu=cpu, arrays=arrays, phase_cycles=end - begin,
                overlaps=overlapping, outputs_checked=5 * dimension, tails_checked=5 * maximum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--build-info", type=Path, help="Reuse a matching build that includes protocol.cc")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case["name"] for case in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [case for case in selected if case["name"] in args.case]
    output = (args.output or ROOT / "tests/results/source-new-array-pipeline" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    keys = {(case["dimension"], case["enabled"], case.get("exit_pending", False), case.get("drain_kind", 0))
            for case in selected if not case.get("protocol")}
    guests = {key: compile_guest(output, args.compiler.resolve(), *key) for key in sorted(keys)}
    if args.build_info:
        tools = json.loads(args.build_info.read_text())
        assert str(HERE / "protocol.cc") in tools["command"], "Build must include the protocol driver"
        for path, digest in tools["source_sha256"].items():
            assert sha256(path) == digest, f"Source changed: {path}; rebuild"
    else:
        tools = build(output / "build", extra_sources=[HERE / "protocol.cc"])
    (output / "metadata.json").write_text(json.dumps(dict(build=tools, guests=list(guests.values())), indent=2) + "\n")
    results = []
    for entry in selected:
        trial = output / entry["name"]; trial.mkdir()
        parameters = dict(
            arrays_per_tile=1, array_rows=entry["dimension"], array_cols=entry["dimension"],
            riscv_vector_length_bits=entry["vlen"], cost_per_mvm_cycles=entry["cost"])
        if not entry.get("use_pipeline_default"):
            parameters["array_pipeline_enabled"] = entry["enabled"]
        case = entry | dict(qemu=str(args.qemu.resolve()), parameters=resolve(parameters))
        if not case.get("protocol"):
            case["elf"] = guests[entry["dimension"], entry["enabled"], entry.get("exit_pending", False), entry.get("drain_kind", 0)]["elf"]
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        script = "protocol.py" if case.get("protocol") else "simulation.py"
        start = time.perf_counter()
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen([tools["sst"], "--num-threads=1", f"--output-json={trial / 'topology.json'}", str(HERE / script)],
                env=os.environ | dict(SST_LIB_PATH=tools["plugin"] + ":" + tools["library"],
                    TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try: process.wait(timeout=120)
            finally:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.wait()
        log = (trial / "simulation.log").read_text()
        if process.returncode: raise RuntimeError(f"{case['name']} failed: {log[-8000:]}")
        result = validate(trial, case, log)
        result["host_wall_seconds"] = time.perf_counter() - start
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}", flush=True)
    by_name = {result["case"]: result for result in results}
    for dimension in (32, 64):
        for vlen in (256, 512):
            prefix = f"array{dimension}-vlen{vlen}-"
            if {prefix + "enabled", prefix + "disabled"} <= by_name.keys():
                assert by_name[prefix + "enabled"]["phase_cycles"] < by_name[prefix + "disabled"]["phase_cycles"]
    if {"budget-1", "array32-vlen256-enabled"} <= by_name.keys():
        assert by_name["budget-1"]["phase_cycles"] == by_name["array32-vlen256-enabled"]["phase_cycles"]
    if {"default-enabled", "array32-vlen256-enabled"} <= by_name.keys():
        assert by_name["default-enabled"]["phase_cycles"] == by_name["array32-vlen256-enabled"]["phase_cycles"]
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS {len(results)} array-pipeline regressions", flush=True)


if __name__ == "__main__": main()
