"""Compare one simulation across SST workers, including receive ownership."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE, ROOT = HERE.parents[1], HERE.parents[2]
sys.path.insert(0, str(SOURCE))
from build import build, load_build_info


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


network_fixture = load("thread_network", SOURCE / "tests/network-instructions/run.py")
dram_fixture = load("thread_dram", SOURCE / "tests/dram-tile/run.py")
CASES = [
    dict(name="mvm-mesh-2x2", mesh_x=2, mesh_y=2, destination=3, size=128, kind=4,
         background_array=False),
    dict(name="reserved-inputs", mesh_x=2, mesh_y=2, destination=3, size=256, kind=8, slots=1),
    dict(name="source-reuse", mesh_x=2, mesh_y=2, destination=3, size=256, kind=0,
         reuse=True, delay=20000, slots=1),
    dict(name="dram-single-slot", mesh_x=2, mesh_y=2, slots=1, delay=12000, dram=True),
    dict(name="mvm-mesh-4x4", mesh_x=4, mesh_y=4, destination=15, size=128, kind=4,
         background_array=False),
]


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_placement(trial, threads):
    topology = json.loads((trial / "topology.json").read_text())
    workers = json.loads((trial / "tile-threads.json").read_text())
    width = json.loads((trial / "case.json").read_text())["mesh_x"]
    components = {item["name"]: item for item in topology["components"]}
    placed = {name: item["partition"] for name, item in components.items()}
    for name, item in components.items():
        if name.startswith("net.tile"):
            tile = int(name.split(".")[1][4:])
        else:
            assert name.startswith("net.router."), name
            x, y = map(int, name.split(".")[-2:])
            tile = y * width + x
        assert placed[name] == dict(rank=0, thread=workers[tile]), (name, placed[name])
    assert all(0 <= worker < threads for worker in workers)
    cross = 0
    for link in topology["links"]:
        left, right = link["left"], link["right"]
        a, b = (end["component"].split(":")[0] for end in (left, right))
        if placed[a] != placed[b]:
            assert a.startswith("net.router.") and b.startswith("net.router."), link
            assert left["latency"] == right["latency"] == "1 ns", link
            cross += 1
    return cross


def run_trial(info, directory, case, threads, *, profile=False, error=None, extra_env=None):
    directory.mkdir()
    if "dram" in case:
        (directory / "weights.bin").symlink_to(case["dram"]["image"])
    write(directory / "case.json", case)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("TILE_COMPONENT_", "TILE_CYCLE_PROFILE", "TILE_EXPERIMENTAL_"))}
    env.update(SST_LIB_PATH=info["plugin"] + ":" + info["library"],
               TILE_COMPONENT_OUTPUT=str(directory), TILE_CYCLE_PROFILE=str(int(profile)),
               PYTHONDONTWRITEBYTECODE="1")
    env.update(extra_env or {})
    command = [info["sst"], f"--num-threads={threads}", "--timing-info=2", "--output-partition",
               f"--profiling-output={directory / 'sst-timing.json'}",
               f"--output-json={directory / 'topology.json'}", str(HERE / "simulation.py")]
    write(directory / "command.json", command)
    start = time.monotonic()
    with (directory / "simulation.log").open("w") as log:
        child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            child.wait(timeout=180)
        finally:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
    wall = time.monotonic() - start
    text = (directory / "simulation.log").read_text()
    if error:
        assert child.returncode != 0 and error in text, text[-4000:]
        return dict(passed=True, expected_error=error)
    if child.returncode:
        raise RuntimeError(text[-5000:])
    statistics = {}
    for line in text.splitlines():
        label, _, payload = line.partition(" ")
        if label.endswith("_STATS"):
            statistics.setdefault(label, []).append(json.loads(payload))
    for records in statistics.values():
        records.sort(key=lambda row: row["component"])
    for cpu in statistics["RISCV_STATS"]:
        for first, last in (("memory_requests", "completed_requests"),
                            ("lsq_enqueued", "lsq_completed"), ("slq_enqueued", "slq_completed"),
                            ("asq_enqueued", "asq_completed")):
            assert cpu[first] == cpu[last]
    if "dram" in case:
        proof = dram_fixture.validate(directory, case["fixture"] | dict(profile=profile),
                                      case["image_sha256"])
    else:
        proof = network_fixture.validate(directory, case | dict(profile=profile))
    profiles = list((directory / "profiles").glob("*-cycles.csv"))
    assert bool(profiles) == profile
    timing = json.loads((directory / "sst-timing.json").read_text())
    result = dict(passed=True, threads=threads, profile=profile, host_seconds=wall,
                  cross_thread_links=validate_placement(directory, threads),
                  statistics=statistics, proof=proof,
                  simulated_time=timing["metadata"]["simulation_time"],
                  spm_sha256={p.name: digest(p) for p in sorted(directory.glob("tile*-spm.bin"))})
    write(directory / "validation.json", result)
    print(f"PASS {directory.parent.name}/{directory.name}: {threads} threads", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-info", type=Path)
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--output", type=Path, default=ROOT / "tests/results/sst-threads" / str(time.time_ns()))
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4])
    parser.add_argument("--case", action="append", choices=[case["name"] for case in CASES])
    args = parser.parse_args()
    if any(n < 1 for n in args.threads) or 1 not in args.threads:
        parser.error("--threads requires a serial reference (1) and positive worker counts")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    info = load_build_info(args.build_info) if args.build_info else build(args.output / "components")
    results, rejection_checks = [], []
    for fixture in CASES:
        if args.case and fixture["name"] not in args.case:
            continue
        directory = args.output / fixture["name"]
        directory.mkdir()
        guests = directory / "guests"
        guests.mkdir()
        if fixture.get("dram"):
            image_hash = dram_fixture.prepare(guests, fixture, args)
            case = json.loads((guests / "case.json").read_text())
            case.update(mesh_x=2, mesh_y=2, fixture=fixture, image_sha256=image_hash)
        else:
            case = network_fixture.configure(fixture, args.qemu)
            case["elfs"] = network_fixture.compile_guests(guests, case, args.compiler)
        hashes = {path: digest(Path(path)) for path in case["elfs"]}
        write(guests / "elf-sha256.json", hashes)
        reference = None
        variants = [n for n in sorted(set(args.threads)) if n <= case["mesh_x"] * case["mesh_y"]]
        for threads in variants:
            # Exercise the full-run profiler separately from ordinary execution.
            for profile in ((False, True) if threads == variants[-1] else (False,)):
                trial = directory / (f"t{threads}" + ("-profile" if profile else ""))
                result = run_trial(info, trial, case, threads, profile=profile)
                if reference is None:
                    reference = result
                for key in ("statistics", "spm_sha256", "simulated_time"):
                    assert result[key] == reference[key], (fixture["name"], threads, profile, key)
                if "message_trace_sha256" in result["proof"]:
                    assert result["proof"]["message_trace_sha256"] == reference["proof"]["message_trace_sha256"]
                results.append(dict(case=fixture["name"], trial=str(trial), **result))
                write(args.output / "results.json", results)
        assert hashes == {path: digest(Path(path)) for path in case["elfs"]}
        if not rejection_checks and max(variants) > 1:
            rejection_checks.append(run_trial(info, directory / "reject-unplaced", case | dict(reject_unplaced_cpu=True),
                2, error="threaded shared-SPM QEMU requires whole-tile placement"))
            rejection_checks.append(run_trial(info, directory / "reject-task-gate", case, 2,
                error="full-lifetime observation scope", extra_env={"TILE_COMPONENT_TRACE_START_TASK": "0"}))
    write(args.output / "validation.json", dict(passed=True, runs=len(results),
          cases=len({r["case"] for r in results}), rejection_checks=rejection_checks,
          identical_statistics=True, identical_memory=True, identical_simulated_time=True))
    print(f"PASS {len(results)} threaded/serial comparisons: {args.output}", flush=True)


if __name__ == "__main__":
    main()
