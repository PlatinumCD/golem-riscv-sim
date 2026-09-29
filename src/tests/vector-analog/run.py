"""Compile and execute custom vector analog instructions through LLVM/QEMU/SST."""
import argparse
from collections import Counter
import csv
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
from build import build, load_build_info
from configuration import resolve


def cases():
    return [dict(name="default"),
            dict(name="vlen128", parameters=dict(riscv_vector_length_bits=128)),
            dict(name="vlen512", parameters=dict(riscv_vector_length_bits=512)),
            dict(name="vlen1024", parameters=dict(riscv_vector_length_bits=1024, array_inflight_bytes=128)),
            dict(name="lmul2", lmul="m2"), dict(name="lmul8", lmul="m8"),
            dict(name="fractional", lmul="mf2"),
            dict(name="vlen128-lmul8", lmul="m8", parameters=dict(riscv_vector_length_bits=128)),
            dict(name="vlen1024-lmul8", lmul="m8", parameters=dict(riscv_vector_length_bits=1024, array_inflight_bytes=1024)),
            dict(name="legacy-cpu-vlen512", cpu_parameters=dict(riscv_vector_length_bits=512)),
            dict(name="spm-slow", parameters=dict(spm_channel_width=1)),
            dict(name="program-17", parameters=dict(cost_per_array_program_cycles=17)),
            dict(name="inflight-5", parameters=dict(array_inflight_bytes=5)),
            dict(name="budget-1", cpu_parameters=dict(instruction_budget=1)),
            dict(name="issue-4", cpu_parameters=dict(issue_width=4))]


def report(log, label):
    lines = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(lines) == 1, (label, lines)
    return lines[0]


def compile_guest(output, compiler, lmul):
    elf = output / f"guest-{lmul}.elf"
    support = HERE.parent / "riscv-qemu"
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog",
        "-fuse-ld=lld", "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0",
        "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none", f"-DLMUL={lmul}",
        "-T", str(support / "scratchpad.ld"), str(support / "start.S"),
        str(HERE / "trap.S"), str(HERE / "guest.c"), "-o", str(elf)]
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
    (output / f"build-{lmul}.log").write_text(process.stdout)
    if process.returncode: print(process.stdout); process.check_returncode()
    return elf


def validate(trial, log):
    parameters = json.loads((trial / "parameters.json").read_text())
    topology = json.loads((trial / "topology.json").read_text())
    array_nodes = [c for c in topology["components"] if c["type"] == "tilecomponents.AnalogArrays"]
    assert len(array_nodes) == 1 and not array_nodes[0].get("subcomponents"), array_nodes
    cpu_nodes = [c for c in topology["components"] if c["type"] == "tilecomponents.RiscvQemu"]
    assert len(cpu_nodes) == 1
    vlen = parameters["riscv_vector_length_bits"]
    assert int(cpu_nodes[0]["params"]["riscv_vector_length_bits"]) == vlen
    assert int(array_nodes[0]["params"]["riscv_vector_length_bits"]) == vlen
    assert int(array_nodes[0]["params"]["array_link_width"]) == parameters["array_link_width"] == vlen // 8
    array_name = array_nodes[0]["name"]
    assert not any(k.startswith("spm_") for k in array_nodes[0]["params"])
    array_links = []
    for link in topology["links"]:
        for end, other in (("left", "right"), ("right", "left")):
            if link[end]["component"] == array_name:
                assert link[end]["port"] == "commands"
                assert link[other]["component"] == "riscv" and link[other]["port"] == "analog_commands"
                array_links.append(link["name"])
    assert len(array_links) == 1, array_links
    cpu, spm, arrays = [report(log, key) for key in ("RISCV_STATS", "SPM_STATS", "ARRAY_STATS")]
    assert cpu["memory_requests"] == cpu["completed_requests"] == spm["accepted"] == spm["completed"]
    assert cpu["analog_commands"] > 0 and cpu["analog_read_bytes"] > 0 and cpu["analog_write_bytes"] > 0
    assert cpu["analog_write_bytes"] == arrays["link_read_bytes"], (cpu, arrays)
    assert cpu["analog_read_bytes"] == arrays["link_write_bytes"], (cpu, arrays)
    assert cpu["analog_commands"] == arrays["completed"]
    assert arrays["peak_buffered_bytes_per_array"] <= parameters["array_inflight_bytes"]
    # Register transfers must not issue SPM requests. Ordinary vle/vse and
    # instruction fetches still consume the banked SPM through the CPU port.
    records = list(csv.DictReader((trial / "array-requests.csv").open()))
    assert not any(r["event"] in ("read_issue", "write_issue") for r in records)
    with (trial / "scratchpad.bin").open("rb") as memory:
        memory.seek(0x100000)
        actual = struct.unpack("<17f", memory.read(68))
    expected = [sum(((r * 19 + c) % 7 - 3) * (c % 5 + 1) for c in range(19)) for r in range(17)]
    assert list(actual) == expected, (actual, expected)
    commands = list(csv.DictReader((trial / "arrays.csv").open()))
    bandwidth = Counter()
    starts, command_cycles = {}, []
    last_release = {r["token"]: int(r["cycle"]) for r in records if r["event"] == "release"}
    for row in commands:
        if row["event"] == "start": starts[row["token"]] = int(row["cycle"])
        if row["event"] == "complete":
            command_cycles.append(int(row["cycle"]) - starts[row["token"]])
        if row["event"] in ("link_read", "link_write"):
            direction = row["event"] if parameters["array_link_duplex"] == "independent" else "shared"
            key = int(row["cycle"]), direction
            bandwidth[key] += int(row["bytes"])
            assert bandwidth[key] <= parameters["array_link_width"]
        if row["event"] == "complete" and row["operation"] == "0" and int(row["element_count"]):
            assert int(row["cycle"]) - last_release[row["token"]] == parameters["cost_per_array_program_cycles"]
    if json.loads((trial / "case.json").read_text())["name"] == "vlen1024-lmul8":
        assert max(int(r["element_count"]) for r in commands) == 256
    program_chunks = sum(r["event"] == "complete" and r["operation"] == "0" and int(r["element_count"]) > 0 for r in commands)
    return dict(cpu=cpu, spm=spm, arrays=arrays, program_chunks=program_chunks,
                array_command_cycles=command_cycles, topology_verified=True,
                vector_length_bits=vlen, array_link_width=vlen // 8,
                peak_link_bytes_per_cycle=max(bandwidth.values(), default=0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append")
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-info", type=Path, help="Reuse a verified component build")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {c["name"] for c in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [c for c in selected if c["name"] in args.case]
    output = (args.output or ROOT / "tests/results/source-new-vector-analog" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    elves = {lmul: compile_guest(output, args.compiler.resolve(), lmul)
             for lmul in sorted({c.get("lmul", "m1") for c in selected})}
    tools = load_build_info(args.build_info) if args.build_info else build(output / "build")
    results = []
    for case in selected:
        trial = output / case["name"]
        trial.mkdir()
        parameters = resolve(dict(array_rows=17, array_cols=19, arrays_per_tile=2) | case.get("parameters", {}),
                             cpu_parameters=case.get("cpu_parameters"))
        case = case | dict(parameters=parameters, elf=str(elves[case.get("lmul", "m1")]), qemu=str(args.qemu.resolve()))
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        (trial / "parameters.json").write_text(json.dumps(parameters, indent=2) + "\n")
        start = time.perf_counter()
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen([tools["sst"], "--num-threads=1",
                f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")],
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
            print(log[-8000:]); raise RuntimeError(f"{case['name']} failed, see {trial}")
        result = dict(case=case["name"], host_wall_seconds=time.perf_counter() - start, **validate(trial, log))
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: {result['cpu']['end_cycle']} cycles", flush=True)
    by_case = {r["case"]: r for r in results}
    # LMUL increases transferred register-group capacity, not bytes per cycle.
    for name, width in (("default", 32), ("lmul2", 32), ("lmul8", 32),
                        ("vlen128-lmul8", 16), ("vlen1024-lmul8", 128)):
        if name in by_case:
            assert by_case[name]["array_link_width"] == width
            assert by_case[name]["peak_link_bytes_per_cycle"] == width
    if {"default", "spm-slow"} <= by_case.keys():
        assert by_case["spm-slow"]["cpu"]["end_cycle"] > by_case["default"]["cpu"]["end_cycle"]
        assert by_case["spm-slow"]["array_command_cycles"] == by_case["default"]["array_command_cycles"]
    if {"default", "program-17"} <= by_case.keys():
        reference, delayed = by_case["default"], by_case["program-17"]
        assert reference["program_chunks"] > 0
        assert delayed["cpu"]["end_cycle"] - reference["cpu"]["end_cycle"] == 17 * reference["program_chunks"]
    # The guest handles illegal-instruction traps and resumes. Cache lookup
    # credit must not leak across those traps or synchronization grants.
    for name in ("budget-1", "issue-4"):
        if {"default", name} <= by_case.keys():
            reference, candidate = by_case["default"]["cpu"], by_case[name]["cpu"]
            for key in ("instructions", "vector_instructions", "icache_fetches", "icache_hits", "icache_misses"):
                assert candidate[key] == reference[key], (name, key, candidate, reference)
            assert candidate["end_cycle"] == reference["end_cycle"], (name, candidate, reference)
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS: {len(results)} LLVM / RVV / analog-array regressions", flush=True)


if __name__ == "__main__": main()
