"""Measure a resident matrix matching one array: size 32/64 x VLEN 256/512."""
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
from configuration import CPU_DEFAULTS, SPM_KEYS, resolve

ICACHE_COUNTERS = tuple("icache_" + name for name in
    ("fetches", "hits", "misses", "fills", "fill_bytes", "evictions", "invalidations", "stall_cycles"))


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compile_guest(output, compiler, dimension, repeats, pipeline):
    elf = output / f"guest-{dimension}.elf"
    support = HERE.parent / "riscv-qemu"
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog",
        "-fuse-ld=lld", "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0",
        "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none",
        f"-DARRAY_DIM={dimension}", f"-DREPEATS={repeats}", f"-DARRAY_PIPELINE_ENABLED={int(pipeline)}",
        "-T", str(support / "scratchpad.ld"), str(support / "start.S"),
        str(HERE / "measurement.S"), str(HERE / "guest.c"), "-o", str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    (output / f"build-guest-{dimension}.log").write_text(process.stdout + process.stderr)
    if process.returncode:
        raise RuntimeError(f"Guest build failed: {process.stderr}")
    disassembly = subprocess.check_output([str(compiler.parent / "llvm-objdump"), "-d", str(elf)], text=True)
    (output / f"guest-{dimension}.asm").write_text(disassembly)
    for instruction in ("mvm.vset", "mvm.vl", "mvm.vs", "vle32.v", "vse32.v"):
        assert instruction in disassembly, instruction
    return dict(elf=str(elf), sha256=sha256(elf), command=command)


def stats(log, label):
    records = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(records) == 1, (label, records)
    return records[0]


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def overlap_cycles(left, right):
    """Intersect two individually non-overlapping sequences of intervals."""
    left, right = sorted(left), sorted(right)
    i = j = total = 0
    while i < len(left) and j < len(right):
        a, b = left[i], right[j]
        total += max(0, min(a[1], b[1]) - max(a[0], b[0]))
        if a[1] <= b[1]: i += 1
        else: j += 1
    return total


def validate(trial, case, log):
    p, repeats = case["parameters"], case["repeats"]
    dimension, vlen = p["array_rows"], p["riscv_vector_length_bits"]
    elements, weights = vlen // 32, dimension * dimension
    cpu, spm, arrays = [stats(log, key) for key in ("RISCV_STATS", "SPM_STATS", "ARRAY_STATS")]
    assert arrays["errors"] == arrays["busy"] == 0, arrays
    assert cpu["memory_requests"] == cpu["completed_requests"] == spm["accepted"] == spm["completed"]
    assert cpu["analog_commands"] == arrays["completed"] == arrays["accepted"]
    assert cpu["analog_write_bytes"] == arrays["link_read_bytes"]
    assert cpu["analog_read_bytes"] == arrays["link_write_bytes"]
    assert arrays["mvms"] == repeats + 1
    assert arrays["peak_active_arrays"] == 1
    topology = json.loads((trial / "topology.json").read_text())
    nodes = {kind: [c for c in topology["components"] if c["type"] == "tilecomponents." + kind]
             for kind in ("RiscvQemu", "AnalogArrays")}
    assert all(len(n) == 1 for n in nodes.values()), nodes
    cpu_node, array_node = nodes["RiscvQemu"][0], nodes["AnalogArrays"][0]
    for node in (cpu_node, array_node):
        assert (str(node["params"]["array_pipeline_enabled"]).lower() in ("true", "1")) == p["array_pipeline_enabled"]
    scratch, = (c for c in topology["components"] if c["type"] == "tilecomponents.Scratchpad")
    converter, = scratch["subcomponents"]
    backend, = converter["subcomponents"]
    assert backend["type"] == "tilecomponents.BankedBackend"
    for key in SPM_KEYS:
        assert int(backend["params"][key]) == p[key], (key, backend["params"][key], p[key])
    assert scratch["params"]["size"] == f'{p["spm_capacity_bytes"]}B'
    assert int(scratch["params"]["scratch_line_size"]) == p["spm_request_bytes"]
    cache_options = {key: value for key, value in CPU_DEFAULTS.items() if key.startswith("instruction_cache_")}
    cache_options.update({key: value for key, value in case.get("cpu_parameters", {}).items()
                          if key.startswith("instruction_cache_")})
    cache_enabled = cache_options["instruction_cache_enabled"]
    for key, value in cache_options.items():
        actual = cpu_node["params"][key]
        if isinstance(value, bool): actual = str(actual).lower() in ("true", "1")
        else: actual = int(actual)
        assert actual == value, (key, actual, value)
    assert cpu["instruction_bytes"] > 0
    if cache_enabled:
        assert cpu["icache_fetches"] == cpu["icache_hits"] + cpu["icache_misses"]
        assert cpu["icache_hits"] > 0 and cpu["icache_misses"] > 0
        assert cpu["icache_misses"] <= cpu["icache_fills"] <= 2 * cpu["icache_misses"]
        assert cpu["fetch_bytes"] == cpu["icache_fill_bytes"] == cpu["icache_fills"] * cache_options["instruction_cache_line_bytes"]
    else:
        assert all(cpu[key] == 0 for key in ICACHE_COUNTERS)
        assert cpu["fetch_bytes"] == cpu["instruction_bytes"]
    assert int(cpu_node["params"]["riscv_vector_length_bits"]) == vlen
    assert int(array_node["params"]["riscv_vector_length_bits"]) == vlen
    assert int(array_node["params"]["array_link_width"]) == vlen // 8 == p["array_link_width"]
    assert int(array_node["params"]["arrays_per_tile"]) == p["arrays_per_tile"] == 1
    assert int(array_node["params"]["array_rows"]) == int(array_node["params"]["array_cols"]) == dimension
    assert not array_node.get("subcomponents")
    array_links = []
    for link in topology["links"]:
        for end, other in (("left", "right"), ("right", "left")):
            if link[end]["component"] == array_node["name"]:
                assert link[end]["port"] == "commands"
                assert link[other]["component"] == cpu_node["name"] and link[other]["port"] == "analog_commands"
                array_links.append(link["name"])
    assert len(array_links) == 1, array_links
    expected = [sum(((r * 3 + c * 5) % 11 - 5) * (c % 5 - 2) for c in range(dimension)) for r in range(dimension)]
    with (trial / "scratchpad.bin").open("rb") as memory:
        memory.seek(0x100000)
        actual = struct.unpack(f"<{dimension * (repeats + 1)}f", memory.read(4 * dimension * (repeats + 1)))
    assert list(actual) == expected * (repeats + 1), "Incorrect guest results"
    (trial / "outputs.json").write_text(json.dumps(dict(expected=expected,
        actual=[actual[i:i + dimension] for i in range(0, len(actual), dimension)]), indent=2) + "\n")

    markers = rows(trial / f"{cpu_node['name']}-tasks.csv")
    assert len(markers) == 8, markers
    endpoints = {}
    for index, marker in enumerate(markers):
        marker = {k: v if k == "event" else int(v) for k, v in marker.items()}
        assert marker["task_id"] == index // 2 and marker["execution_id"] == 0
        assert marker["event"] == ("start" if index % 2 == 0 else "finish")
        assert marker["memory_requests"] == marker["completed_requests"]
        endpoints[marker["task_id"], marker["event"]] = marker
    def interval(start, finish):
        assert finish["cycle"] > start["cycle"]
        return dict(start_cycle=start["cycle"], end_cycle=finish["cycle"],
            **{("cycles" if k == "cycle" else k): finish[k] - start[k]
               for k in start if k not in ("event", "task_id", "execution_id")})
    phases = {name: interval(endpoints[i, "start"], endpoints[i, "finish"])
              for i, name in enumerate(("empty_marker", "programming", "first_mvm", "warm_batch"))}
    phases["cold"] = interval(endpoints[1, "start"], endpoints[2, "finish"])
    for phase in phases.values():
        if cache_enabled:
            assert phase["icache_fetches"] == phase["icache_hits"] + phase["icache_misses"]
            assert phase["icache_misses"] <= phase["icache_fills"] <= 2 * phase["icache_misses"]
            assert phase["fetch_bytes"] == phase["icache_fill_bytes"] == phase["icache_fills"] * cache_options["instruction_cache_line_bytes"]
        else:
            assert all(phase[key] == 0 for key in ICACHE_COUNTERS)
            assert phase["fetch_bytes"] == phase["instruction_bytes"]
    if cache_enabled:
        assert phases["warm_batch"]["icache_hits"] > 0
        # The MVM function is warm, but the batch loop can enter a new main()
        # code line. Report those compulsory misses without hiding their cost.
    assert phases["empty_marker"]["analog_commands"] == 0
    assert phases["empty_marker"]["read_bytes"] == phases["empty_marker"]["write_bytes"] == 0
    assert phases["programming"]["analog_write_bytes"] == weights * 4
    assert phases["programming"]["analog_read_bytes"] == 0
    assert phases["programming"]["analog_commands"] == weights // elements
    per_mvm_commands = 2 * dimension // elements + 1
    for name, count in (("first_mvm", 1), ("warm_batch", repeats)):
        phase = phases[name]
        assert phase["analog_commands"] == per_mvm_commands * count
        assert phase["analog_write_bytes"] == phase["analog_read_bytes"] == dimension * 4 * count
        assert phase["vector_read_bytes"] == phase["vector_write_bytes"] == dimension * 4 * count
        assert phase["vector_memory_beats"] == 2 * dimension // elements * count

    trace = rows(trial / "arrays.csv")
    bandwidth, starts, completed = Counter(), {}, []
    for row in trace:
        cycle = int(row["cycle"])
        if row["event"] == "start": starts[row["token"]] = row
        if row["event"] == "complete":
            start = starts.pop(row["token"])
            assert row["array"] == start["array"] and row["operation"] == start["operation"]
            completed.append(dict(array=int(row["array"]), operation=int(row["operation"]),
                start_cycle=int(start["cycle"]), end_cycle=cycle,
                cycles=cycle - int(start["cycle"]), elements=int(row["element_count"])))
        if row["event"] in ("link_read", "link_write"):
            bandwidth[cycle] += int(row["bytes"])
    assert not starts and len(completed) == cpu["analog_commands"]
    assert {c["array"] for c in completed} == {0}
    assert max(bandwidth.values()) == vlen // 8
    assert all(c["elements"] == elements for c in completed if c["operation"] != 2)
    assert all(c["cycles"] == p["cost_per_mvm_cycles"] for c in completed if c["operation"] == 2)
    compute_intervals = sorted((c["start_cycle"], c["end_cycle"]) for c in completed if c["operation"] == 2)
    assert all(left[1] <= right[0] for left, right in zip(compute_intervals, compute_intervals[1:])), "One compute unit cannot execute two MVMs concurrently"
    for name, phase in phases.items():
        commands = [c for c in completed if phase["start_cycle"] <= c["start_cycle"] <= c["end_cycle"] <= phase["end_cycle"]]
        assert len(commands) == phase["analog_commands"], (name, len(commands), phase)
        phase["array_commands_by_operation"] = {str(op): sum(c["operation"] == op for c in commands) for op in range(4)}
        phase["array_compute_cycles"] = sum(c["cycles"] for c in commands if c["operation"] == 2)
        phase["array_transfer_service_cycles"] = sum(c["cycles"] for c in commands if c["operation"] != 2)
        computes = [(c["start_cycle"], c["end_cycle"]) for c in commands if c["operation"] == 2]
        for operation, label in ((1, "input"), (3, "output")):
            transfers = [(c["start_cycle"], c["end_cycle"]) for c in commands if c["operation"] == operation]
            phase[f"array_{label}_compute_overlap_cycles"] = overlap_cycles(computes, transfers)
    assert phases["warm_batch"]["array_commands_by_operation"]["0"] == 0
    warm = phases["warm_batch"]
    assert phases["first_mvm"]["array_commands_by_operation"]["0"] == 0
    if p["array_pipeline_enabled"] and cache_enabled and repeats > 1:
        assert warm["array_output_compute_overlap_cycles"] > 0, "Pipeline run must actually overlap output handling and computation"
    if not p["array_pipeline_enabled"]:
        assert all(phase["array_input_compute_overlap_cycles"] == phase["array_output_compute_overlap_cycles"] == 0 for phase in phases.values())
    return dict(case=case["name"], array_dimension=dimension, matrix_dimension=dimension, vlen_bits=vlen,
        link_bytes_per_cycle=vlen // 8, arrays_installed=1, arrays_used=1, repeats=repeats,
        instruction_cache_enabled=cache_enabled, instruction_cache_options=cache_options,
        array_pipeline_enabled=p["array_pipeline_enabled"],
        spm_parameters={key: value for key, value in p.items() if key.startswith("spm_")},
        phases=phases, warm_cycles_per_mvm=warm["cycles"] / repeats,
        cold_cycles=phases["cold"]["cycles"], programming_cycles=phases["programming"]["cycles"],
        first_mvm_cycles=phases["first_mvm"]["cycles"], cpu=cpu, spm=spm, arrays=arrays,
        numerical_results_checked=dimension * (repeats + 1), topology_verified=True,
        peak_link_bytes_per_cycle=max(bandwidth.values()))


def write_report(output, results, repeats):
    spm = results[0]["spm_parameters"]
    assert all(r["spm_parameters"] == spm for r in results)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    with (output / "summary.csv").open("w") as stream:
        fields = ["case", "array_dimension", "matrix_dimension", "vlen_bits", "link_bytes_per_cycle", "arrays_installed", "arrays_used",
                  "array_pipeline_enabled", "instruction_cache_enabled", "programming_cycles", "first_mvm_cycles", "cold_cycles", "warm_cycles_per_mvm"]
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    with (output / "phases.csv").open("w") as stream:
        fields = ["case", "phase", "cycles", "instructions", "vector_instructions", "issue_cycles",
                  "read_bytes", "write_bytes", "instruction_bytes", "fetch_bytes", "memory_requests", "analog_commands",
                  "analog_read_bytes", "analog_write_bytes", "array_compute_cycles", "array_transfer_service_cycles",
                  "array_input_compute_overlap_cycles", "array_output_compute_overlap_cycles",
                  *ICACHE_COUNTERS]
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for result in results:
            for name, phase in result["phases"].items():
                writer.writerow(dict(case=result["case"], phase=name, **phase))
    lines = ["# Single-tile runtime: one array with a matching resident matrix", "",
        f"Actual LLVM/QEMU/SST execution; one physical array, one cold MVM and {repeats} additional MVMs with resident weights.",
        f"Array pipeline: {'enabled' if results[0]['array_pipeline_enabled'] else 'disabled'}.",
        "The 32×32 array computes a 32×32 matrix-vector product; the 64×64 array computes a 64×64 product. These are different workload sizes.",
        "All cycles are simulated at 1 GHz (1,000 cycles = 1 µs).", "",
        "| Array / matrix | VLEN | Link B/cycle | Programming µs | First MVM µs | Cold total µs | Warm µs/MVM |",
        "|---|---:|---:|---:|---:|---:|---:|"]
    for r in results:
        lines.append(f"| {r['array_dimension']}×{r['array_dimension']} | {r['vlen_bits']} | "
            f"{r['link_bytes_per_cycle']} | {r['programming_cycles']/1000:.3f} | "
            f"{r['first_mvm_cycles']/1000:.3f} | {r['cold_cycles']/1000:.3f} | {r['warm_cycles_per_mvm']/1000:.3f} |")
    lines += ["", "Programming times include generated CPU loops and instruction fetches through the configured cache path. "
        "The 32×32 case programs 1,024 float32 weights (4,096 bytes); the 64×64 case programs 4,096 weights (16,384 bytes). "
        "The saved guest-32.asm and guest-64.asm record the compiler output; phases.csv separates CPU instructions and array service costs."]
    lines += ["", "Each tile has one physical array, addressed as array 0. All matrix weights remain resident throughout the first MVM "
        "and the repeated batch, with no reprogramming or partial-output accumulation. A 32×32 MVM performs 1,024 multiply-accumulates; "
        "a 64×64 MVM performs 4,096. The larger array has four times the weight capacity and computes four times the matrix work per MVM.", "",
        "The model charges 100 cycles per array MVM at either size and zero additional programming "
        "settling cycles per chunk. Each MVM performs one array execution, so modeled compute latency is 100 cycles in both cases.", "",
        "Each 32×32 MVM transfers 128 input and 128 output bytes; each 64×64 MVM transfers 256 of each. "
        "All transfers use e32/m1; VLEN changes the chunk size and physical link width together. Each execution reloads its input from SPM "
        "and writes its result to a separate SPM output buffer.", "",
        "Timing includes CPU instructions, instruction fetches, ordinary SPM vector loads/stores, and analog commands. "
        "Initialization and numerical verification are outside the measured phases. All outputs are stored to SPM within the measured kernels. "
        "Cold timing runs continuously from programming start through first-MVM finish, including the intervening markers. Warm timing is one resident-weight batch divided by its MVM count.", "",
        "Guest task markers are timestamped by SST. Their instructions are included; no estimated overhead is subtracted. Empty-marker controls: " +
        ", ".join(f"{r['case']}: {r['phases']['empty_marker']['cycles']} cycles" for r in results) + ". "
        "The CSV trace records cumulative CPU counters at each marker. Array service-cycle totals in phases.csv exclude CPU-side transport, fetch, and scheduling costs.", "",
        f"Fixed SPM: {spm['spm_capacity_bytes'] / 2**20:g} MiB, {spm['spm_banks']} banks of "
        f"{spm['spm_bank_width']} bytes ({spm['spm_bank_width'] * 8} bits), "
        f"{spm['spm_read_ports_per_bank']} read and {spm['spm_write_ports_per_bank']} write port(s) per bank, "
        f"{spm['spm_channels']} channels of {spm['spm_channel_width']} bytes/cycle, {spm['spm_request_bytes']}-byte requests. "
        "Array buffering: 64 bytes per array; shared bidirectional link. CPU: issue width 1, instruction budget 256. Contiguous active RVV elements use pre-access beats up to VLEN/8 bytes, "
        "split into concurrent request-line fragments and serviced under the fixed bank/channel limits. Doubling VLEN reduces round trips but does not double SPM bank bandwidth. "
        "This is an ideal functional/timing model, not a measured physical chip.", "",
        "All guest and host numerical checks, operation counts, transferred bytes, link bandwidth, and CPU-only array connectivity passed. "
        "See results.json for counters and host simulation wall time, summary.csv for the four-way comparison, phases.csv for timing breakdowns, "
        "and each case directory for the actual topology, traces, and output vectors.", ""]
    if results[0]["array_pipeline_enabled"]:
        lines += ["Pipeline mode keeps at most two jobs resident. It starts the next MVM before draining the previous result. "
            "The array snapshots inputs, retains results in a two-slot FIFO, and overlaps its single compute engine with input/output work. "
            "CPU memory instructions remain blocking, with grouped RVV beats; input preparation and output handling share CPU and link resources. "
            "Computation latency remains 100 cycles; two MVM computations never execute concurrently.", "",
            "Warm-batch array transfer/compute overlap: " + "; ".join(
                f"{r['case']}: {r['phases']['warm_batch']['array_input_compute_overlap_cycles']} input cycles, "
                f"{r['phases']['warm_batch']['array_output_compute_overlap_cycles']} output cycles" for r in results) + ". "
            "These counters cover register-to-array transfers; CPU input preparation can also overlap computation before an array transfer starts.", ""]
    else:
        lines += ["Pipeline mode is disabled: each MVM loads its input, waits for computation, and drains its output before starting the next MVM.", ""]
    if results[0]["instruction_cache_enabled"]:
        lines += ["Instruction cache: 8 KiB, two ways, 64-byte lines, least recently used replacement, "
            "one-cycle hits including the issue cycle. Misses fetch real SPM lines. Data accesses remain uncached. "
            "The warm MVM function reuses cached instructions; first entry to its batch-loop scaffold may still miss.", "",
            "Warm-batch instruction-cache counters: " + "; ".join(
                f"{result['case']}: {result['phases']['warm_batch']['icache_hits']} hits, "
                f"{result['phases']['warm_batch']['icache_misses']} misses, "
                f"{result['phases']['warm_batch']['icache_fill_bytes']} fill bytes" for result in results) + ".", ""]
    else:
        lines += ["Instruction cache disabled by --uncached: instruction fetches go directly to SPM. "
                  "Data accesses remain uncached. This is the control for measuring instruction-cache impact.", ""]
    (output / "report.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=1000)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--build-info", type=Path, help="Reuse build.json only when the current component source set and hashes match")
    parser.add_argument("--uncached", action="store_true", help="Disable the instruction cache for a controlled comparison")
    parser.add_argument("--pipeline", action=argparse.BooleanOptionalAction,
                        default=resolve()["array_pipeline_enabled"],
                        help="Enable array pipelining and schedule two successive MVMs in flight (default: enabled; --no-pipeline selects the blocking control)")
    parser.add_argument("--spm-banks", type=int, default=resolve()["spm_banks"],
                        help="SPM bank count; retains four-byte banks and the configured capacity")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 1000: parser.error("--repeats must be between 1 and 1000")
    try: resolve(dict(spm_banks=args.spm_banks))
    except ValueError as error: parser.error(str(error))
    output = (args.output or ROOT / "tests/results/source-new-single-tile-runtime" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    elves = {d: compile_guest(output, args.compiler.resolve(), d, args.repeats, args.pipeline) for d in (32, 64)}
    if args.build_info:
        tools = json.loads(args.build_info.read_text())
        components = SOURCE / "components"
        current_sources = {str(path.resolve()) for path in components.rglob("*") if path.suffix in (".h", ".cc")}
        recorded_sources = {path for path in tools["source_sha256"]
                            if Path(path).is_relative_to(components) and Path(path).suffix in (".h", ".cc")}
        assert current_sources == recorded_sources, "Component source set changed; rebuild before reusing build.json"
        assert str(components / "riscv-qemu/instructionCache.h") in recorded_sources, "Build predates instruction-cache support; rebuild"
        assert str(components / "riscv-qemu/instructionCache.cc") in recorded_sources, "Build predates instruction-cache support; rebuild"
        for path, digest in tools["source_sha256"].items():
            if Path(path).suffix in (".h", ".cc") or Path(path).name in ("build.py", "prepare_controller.py"):
                assert sha256(path) == digest, f"Build input changed: {path}; rebuild"
    else:
        tools = build(output / "build")
    cpu_options = {key: value for key, value in CPU_DEFAULTS.items() if key.startswith("instruction_cache_")}
    cpu_options["instruction_cache_enabled"] = not args.uncached
    metadata = dict(repeats=args.repeats, array_pipeline_enabled=args.pipeline, spm_banks=args.spm_banks,
        guests=elves, build=tools, cpu_parameters=cpu_options,
        qemu=str(args.qemu.resolve()), qemu_sha256=sha256(args.qemu.resolve()),
        compiler_version=subprocess.check_output([str(args.compiler.resolve()), "--version"], text=True),
        benchmark_sha256={p.name: sha256(p) for p in HERE.iterdir() if p.is_file()})
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    results = []
    for dimension in (32, 64):
        for vlen in (256, 512):
            name = f"array{dimension}-vlen{vlen}"
            trial = output / name
            trial.mkdir()
            case = dict(name=name, repeats=args.repeats, elf=elves[dimension]["elf"], qemu=str(args.qemu.resolve()),
                cpu_parameters=cpu_options,
                parameters=resolve(dict(array_rows=dimension, array_cols=dimension,
                    arrays_per_tile=1, riscv_vector_length_bits=vlen, array_pipeline_enabled=args.pipeline,
                    spm_banks=args.spm_banks)))
            (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
            (trial / "parameters.json").write_text(json.dumps(case["parameters"], indent=2) + "\n")
            start = time.perf_counter()
            with (trial / "simulation.log").open("w") as log:
                process = subprocess.Popen([tools["sst"], "--num-threads=1", f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")],
                    env=os.environ | dict(SST_LIB_PATH=tools["plugin"] + ":" + tools["library"],
                        TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try: process.wait(timeout=300)
                finally:
                    try: os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    process.wait()
            log = (trial / "simulation.log").read_text()
            if process.returncode:
                raise RuntimeError(f"{name} failed: {log[-8000:]}")
            result = validate(trial, case, log)
            result["host_wall_seconds"] = time.perf_counter() - start
            results.append(result)
            write_report(output, results, args.repeats)
            print(f"PASS {name}: cold {result['cold_cycles']} cycles; warm {result['warm_cycles_per_mvm']:.2f} cycles/MVM", flush=True)
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results),
        outputs_checked=sum(r["numerical_results_checked"] for r in results)), indent=2) + "\n")
    print(f"PASS four-way single-tile benchmark: {output / 'report.md'}", flush=True)


if __name__ == "__main__": main()
