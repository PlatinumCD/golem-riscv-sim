"""Validate four real RISC-V tiles transferring data through banked SPM and Mordred."""
import argparse
import hashlib
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
from profiling import enabled as profiling_enabled
from validate import validate_trial


def cases():
    cpu = dict(load_store_queue_depth=8, analog_command_queue_depth=4)
    normal = dict(cpu_parameters=cpu, parameters=dict(spm_banks=4,
        cpu_spm_banks=[0, 1, 2, 3], router_spm_banks=[2, 3]))
    return [normal | dict(name="shared4"),
            normal | dict(name="remapped", parameters=normal["parameters"] | dict(router_spm_banks=[0, 2])),
            normal | dict(name="width8", parameters=normal["parameters"] | dict(spm_bank_width=8)),
            normal | dict(name="request4", parameters=normal["parameters"] | dict(spm_bank_width=8, spm_request_bytes=4),
                router_parameters=dict(memory_queue_depth=2)),
            normal | dict(name="vlen512", parameters=normal["parameters"] | dict(riscv_vector_length_bits=512)),
            normal | dict(name="budget1", cpu_parameters=cpu | dict(instruction_budget=1)),
            normal | dict(name="cpu-denied-fetch", parameters=normal["parameters"] | dict(cpu_spm_banks=[2, 3]),
                expected_cpu_rejection=True)]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compile_guests(directory, compiler, case):
    directory.mkdir(parents=True, exist_ok=True)
    elfs = []
    for identity in range(4):
        elf = directory / f"tile{identity}.elf"
        command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog", "-fuse-ld=lld",
            "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0", "-O2", "-ffreestanding",
            "-fno-builtin", "-fno-vectorize", "-fno-slp-vectorize", "-nostdlib", "-static",
            "-Wl,--no-relax", "-Wl,--build-id=none", f"-DTILE_ID={identity}",
            f"-DBANKS={case['parameters'].get('spm_banks', 4)}",
            f"-DBANK_WIDTH={case['parameters'].get('spm_bank_width', 4)}",
            f"-DROUTER_BANK0={case['parameters']['router_spm_banks'][0]}",
            f"-DROUTER_BANK1={case['parameters']['router_spm_banks'][1]}",
            "-T", str(HERE / "scratchpad.ld"), str(HERE / "start.S"), str(HERE / "guest.c"), "-o", str(elf)]
        with (directory / f"tile{identity}-build.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=90, check=True)
        (directory / f"tile{identity}-build.json").write_text(json.dumps(dict(command=command, sha256=sha(elf)), indent=2) + "\n")
        elfs.append(str(elf))
    return elfs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", action="append")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/source_new-mordred-spm")
    parser.add_argument("--reuse-build", action="store_true")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--build-info", type=Path)
    args = parser.parse_args()
    selected = [case for case in cases() if not args.case or case["name"] in args.case]
    if args.case and set(args.case) - {case["name"] for case in selected}:
        parser.error("unknown case")
    output = (args.output or ROOT / "tests/results/source-new-mordred-spm" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(output, flush=True)
    build_dir = args.build_dir.resolve()
    if args.build_info:
        info = load_build_info(args.build_info, extra_sources=[HERE / "initiator.cc"])
        build_dir = Path(info["plugin"])
    elif args.reuse_build:
        info = json.loads((build_dir / "build.json").read_text())
        # Python fixture/validator edits do not change compiled code. Component
        # inputs and the builder itself must still match the compiled manifest.
        for name, digest in info["source_sha256"].items():
            path = Path(name)
            if "/components/" in name or path == SOURCE / "build.py" or path.suffix in (".cc", ".h"):
                assert sha(path) == digest, f"Changed build input {path}; run without --reuse-build"
    else:
        info = build(build_dir, extra_sources=[HERE / "initiator.cc"])
    assert str(HERE / "initiator.cc") in info["command"], "Build must include the test-only initiator.cc"
    qemu, compiler = args.qemu.resolve(), args.compiler.resolve()
    binary_hashes = {str(path): sha(path) for path in
                     (build_dir / "libtilecomponents.so", build_dir / "libmordred.so", qemu, compiler)}
    inputs = [p for p in HERE.rglob("*") if p.is_file() and p.suffix in (".py", ".c", ".cc", ".h", ".S", ".ld")]
    source_hashes = {str(path): sha(path) for path in inputs}
    guest_builds, results = {}, []
    for case in selected:
        key = (case["parameters"].get("spm_bank_width", 4), *case["parameters"]["router_spm_banks"])
        if key not in guest_builds:
            guest_builds[key] = compile_guests(output / ("guest-" + "-".join(map(str, key))), compiler, case)
        trial = output / case["name"]
        trial.mkdir()
        case = case | dict(elfs=guest_builds[key], qemu=str(qemu), cycle_profiling=profiling_enabled())
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        command = [info["sst"], "--num-threads=1", f"--add-lib-path={build_dir}",
                   f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")]
        environment = os.environ | dict(TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1")
        environment.pop("TILE_COMPONENT_TRACE_START_TASK", None)
        environment.pop("TILE_COMPONENT_PROGRAM_PROOF", None)
        started = time.monotonic()
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                process.wait(timeout=180)
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
        if process.returncode and not case.get("expected_cpu_rejection"):
            print((trial / "simulation.log").read_text()[-8000:])
            raise RuntimeError(f"SST failed in {case['name']}: {process.returncode}")
        checks = validate_trial(trial, case, process.returncode)
        result = dict(name=case["name"], passed=True, checks=checks,
                      host_seconds=time.monotonic() - started, command=command)
        results.append(result)
        (trial / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"PASS {case['name']}: shared bank access validation", flush=True)
    assert source_hashes == {name: sha(name) for name in source_hashes}, "Test sources changed during run"
    assert binary_hashes == {name: sha(name) for name in binary_hashes}, "Binaries changed during run"
    comparisons = {}
    by_name = {result["name"]: result["checks"] for result in results}
    if {"shared4", "budget1"} <= by_name.keys():
        # QEMU grant batching must not change the simulated memory/NoC timing.
        fields = ("end_cycle", "instructions", "vector_instructions", "issue_cycles",
                  "memory_requests", "completed_requests", "read_bytes", "write_bytes",
                  "icache_hits", "icache_misses", "lsq_stall_cycles", "asq_stall_cycles")
        for normal, single in zip(by_name["shared4"]["tiles"], by_name["budget1"]["tiles"]):
            assert {key: normal["cpu"][key] for key in fields} == {key: single["cpu"][key] for key in fields}
        assert by_name["shared4"]["endpoints"] == by_name["budget1"]["endpoints"]
        assert by_name["shared4"]["clients"] == by_name["budget1"]["clients"]
        assert by_name["shared4"]["trace_signatures"] == by_name["budget1"]["trace_signatures"]
        comparisons["instruction_budgets_1_and_256_preserve_timing"] = True
    summary = dict(passed=True, configurations=len(results), results=results,
                   comparisons=comparisons, build=info, binary_sha256=binary_hashes, source_sha256=source_hashes)
    (output / "validation.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"PASS: {output / 'validation.json'}", flush=True)


if __name__ == "__main__":
    main()
