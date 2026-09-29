"""Validate pre-access RVV ranges, bank service, fallback, and precise faults."""
import argparse
from collections import Counter, defaultdict
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

BASE, INPUT, OUTPUT, END = 0x90000000, 0x90101000, 0x90104000, 0x90200000


def cases():
    result = [dict(name=f"vlen{vlen}-e{sew}-m1", vlen=vlen, sew=sew, lmul="m1")
              for vlen in (256, 512) for sew in (8, 16, 32, 64)]
    result += [dict(name=f"vlen{vlen}-e{sew}-{lmul}", vlen=vlen, sew=sew, lmul=lmul)
               for vlen in (256, 512) for sew, lmul in ((32, "m2"), (8, "m8"))]
    result += [dict(name="one-bank", vlen=256, sew=32, lmul="m1", parameters=dict(spm_banks=1)),
               dict(name="channel-8", vlen=256, sew=32, lmul="m1", parameters=dict(spm_channel_width=8))]
    return result


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compile_guest(output, compiler, sew, lmul):
    elf = output / f"guest-e{sew}-{lmul}.elf"
    support = HERE.parent / "riscv-qemu"
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog",
        "-fuse-ld=lld", "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0",
        "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-ffreestanding", "-fno-builtin",
        "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none", f"-DSEW={sew}", f"-DLMUL={lmul}",
        "-T", str(support / "scratchpad.ld"), str(support / "start.S"),
        str(HERE.parent / "single-tile-runtime/measurement.S"), str(HERE / "trap.S"), str(HERE / "guest.c"),
        "-o", str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    elf.with_suffix(".build.log").write_text(process.stdout + process.stderr)
    if process.returncode: raise RuntimeError(process.stderr)
    return dict(elf=str(elf), command=command, sha256=sha256(elf))


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, label):
    found = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(found) == 1, (label, found)
    return found[0]


def pattern(index):
    return (index * 13 + 7) & 255


def validate(trial, case, log):
    p = case["parameters"]
    cpu, spm, peer = (stats(log, label) for label in ("RISCV_STATS", "SPM_STATS", "RISCV_PEER_RESULT"))
    assert peer["passed"] and peer["verified_bytes"] == 40
    assert cpu["memory_requests"] == cpu["completed_requests"]
    assert spm["accepted"] == spm["completed"]
    vlenb, element = case["vlen"] // 8, case["sew"] // 8
    total = vlenb * int(case["lmul"][1:])
    maximum = total // element
    initialized = max(total, 2 * vlenb) + 8
    expected = [bytearray([0xa5] * initialized) for _ in range(10)]
    expected[0][:total] = bytes(pattern(i) for i in range(total))
    expected[1][:4] = bytes(pattern(i) for i in range(4))
    expected[2][:total - element] = expected[0][:total - element]
    for lane in range(maximum):
        if lane % 2 == 0:
            expected[3][lane * element:(lane + 1) * element] = expected[0][lane * element:(lane + 1) * element]
    expected[4][1:total + 1] = bytes(pattern(i + 1) for i in range(total))
    expected[5][element:total] = expected[0][element:total]
    for slot in (6, 7):
        expected[slot][:2 * element] = bytes([0x3c] * (2 * element))
        expected[slot][2 * element:4 * element] = (90).to_bytes(element, "little") * 2
    expected[8][:total] = bytes(pattern(2 * (i // element) * element + i % element) for i in range(total))
    expected[9][:2 * vlenb] = bytes(pattern(i) for i in range(2 * vlenb))
    with (trial / "scratchpad.bin").open("rb") as memory:
        for slot in range(10):
            memory.seek(OUTPUT - BASE + slot * 0x800)
            actual = memory.read(initialized)
            assert actual == expected[slot], (slot, list(actual), list(expected[slot]))
        memory.seek(0x120000)
        control = struct.unpack("<14Q", memory.read(112))
        assert control == (case["sew"], maximum, total, initialized, 3,
                           5, END, 2, 7, END, 2, 5, 0x90119010, 4), control
        memory.seek(END - BASE - 2 * element)
        assert memory.read(2 * element) == bytes(pattern(i) for i in range(2 * element))
        memory.seek(0x118000)
        assert struct.unpack("<2I", memory.read(8)) == (0x01d00513, 0x00008067)
        memory.seek(0x110000)
        assert struct.unpack("<8I", memory.read(32)) == tuple(range(0x1100, 0x1104)) + (0xffffffff,) * 4

    markers = rows(trial / "riscv-tasks.csv")
    assert len(markers) == 26
    phases = {}
    for phase in range(1, 14):
        start, finish = markers[2 * (phase - 1):2 * phase]
        assert start["event"] == "start" and finish["event"] == "finish"
        assert int(start["task_id"]) == int(finish["task_id"]) == phase
        phases[phase] = (int(start["cycle"]), int(finish["cycle"]))
        assert phases[phase][0] < phases[phase][1]
    memory_trace = rows(trial / "riscv-memory.csv")
    issues = [row for row in memory_trace if row["event"] == "issue"]
    ready = [row for row in memory_trace if row["event"] == "ready"]
    assert len(issues) == len(ready)
    vector_issues = [row for row in issues if row["vector"] == "1"]
    assert len(vector_issues) == cpu["vector_memory_beats"]
    for write, key in (("0", "vector_read_bytes"), ("1", "vector_write_bytes")):
        assert sum(int(row["bytes"]) for row in vector_issues if row["write"] == write) == cpu[key]

    def in_phase(row, phase):
        return phases[phase][0] <= int(row["cycle"]) <= phases[phase][1]

    def accesses(phase, write, address, length):
        return [row for row in issues if in_phase(row, phase) and row["write"] == str(write)
                and address <= int(row["address"]) < address + length]

    for write, address in ((0, INPUT), (1, OUTPUT)):
        records = accesses(1, write, address, total)
        assert len(records) == total // vlenb
        assert all(row["vector"] == "1" and int(row["bytes"]) == vlenb for row in records)
        assert [int(row["address"]) for row in records] == list(range(address, address + total, vlenb))
    for write, address in ((0, INPUT), (1, OUTPUT + 0x800)):
        records = accesses(2, write, address, 4)
        assert len(records) == 1 and int(records[0]["bytes"]) == 4 and records[0]["vector"] == "0"
    for write, address in ((0, INPUT), (1, OUTPUT + 3 * 0x800)):
        records = accesses(4, write, address, total)
        assert sum(int(row["bytes"]) for row in records) == maximum // 2 * element
        assert all(int(row["bytes"]) == element for row in records), "mask gaps were merged"
    for phase, slot in ((3, 2), (6, 5)):
        for write, address in ((0, INPUT), (1, OUTPUT + slot * 0x800)):
            records = accesses(phase, write, address, total)
            assert sum(int(row["bytes"]) for row in records) == total - element
            if phase == 6: assert min(int(row["address"]) for row in records) == address + element
    for write, address in ((0, INPUT + 1), (1, OUTPUT + 4 * 0x800 + 1)):
        records = accesses(5, write, address, total)
        assert sum(int(row["bytes"]) for row in records) == total
        if element > 1: assert all(row["vector"] == "0" for row in records), "element misalignment should use fallback"
    for phase, write in ((7, 0), (8, 0), (9, 1)):
        records = accesses(phase, write, END - 2 * element, 4 * element)
        assert sum(int(row["bytes"]) for row in records) == 2 * element
        assert all(int(row["address"]) + int(row["bytes"]) <= END for row in records)
    strided = accesses(10, 0, INPUT, 2 * total)
    assert len(strided) == maximum and all(int(row["bytes"]) == element and row["vector"] == "0" for row in strided)
    for write, address in ((0, INPUT), (1, OUTPUT + 9 * 0x800)):
        records = accesses(11, write, address, 2 * vlenb)
        assert len(records) == 2 and all(int(row["bytes"]) == vlenb and row["vector"] == "1" for row in records)
    code_writes = accesses(12, 1, 0x90118000, 8)
    assert code_writes and code_writes[0]["vector"] == "0", "translated code must initially use safe fallback"
    assert {int(row["address"]) for row in code_writes} == {0x90118000, 0x90118004}
    pmp_reads = accesses(13, 0, 0x90119000, 32)
    assert [int(row["address"]) for row in pmp_reads] == list(range(0x90119000, 0x90119010, 4))
    assert all(row["vector"] == "0" and int(row["bytes"]) == 4 for row in pmp_reads)
    peer_vector, = (row for row in issues if row["write"] == "1" and int(row["address"]) == 0x90100044)
    assert peer_vector["vector"] == "1" and int(peer_vector["bytes"]) == 32

    bank_trace = rows(next(trial.glob("scratchpad*.csv")))
    accepted, completed, service = {}, {}, defaultdict(list)
    port_use, channel_bytes = set(), Counter()
    for row in bank_trace:
        identity = row["id"]
        if row["event"] == "accepted": accepted[identity] = row
        elif row["event"] == "completed": completed[identity] = row
        else:
            assert row["event"] == "service"
            service[identity].append(row)
            cycle, address, count = int(row["cycle"]), int(row["address"]), int(row["bytes"])
            bank, port = int(row["bank"]), int(row["port"])
            assert bank == address // p["spm_bank_width"] % p["spm_banks"]
            assert count <= p["spm_bank_width"] - address % p["spm_bank_width"]
            port_key = (cycle, bank, port, row["write"])
            assert port_key not in port_use; port_use.add(port_key)
            channel_bytes[cycle, row["channel"]] += count
            assert channel_bytes[cycle, row["channel"]] <= p["spm_channel_width"]
    assert len(accepted) == len(completed) == spm["accepted"]
    for identity, row in accepted.items():
        assert sum(int(beat["bytes"]) for beat in service[identity]) == int(row["bytes"])
        assert int(completed[identity]["cycle"]) > max(int(beat["cycle"]) for beat in service[identity])
    full = [row for row in accepted.values() if in_phase(row, 1) and row["requestor"].endswith(":qemu_memory")
            and ((row["write"] == "0" and INPUT - BASE <= int(row["address"]) < INPUT - BASE + total)
                 or (row["write"] == "1" and OUTPUT - BASE <= int(row["address"]) < OUTPUT - BASE + total))]
    assert len(full) == 2 * total // 32 and all(int(row["bytes"]) == 32 for row in full)
    expected_cycles = 8 if case["name"] == "one-bank" else 4 if case["name"] == "channel-8" else 1
    for request in full:
        beats = service[request["id"]]
        assert len(beats) == 8 and all(int(beat["bytes"]) == 4 for beat in beats)
        assert len({int(beat["cycle"]) for beat in beats}) == expected_cycles
        if p["spm_banks"] == 8: assert {int(beat["bank"]) for beat in beats} == set(range(8))
    if case["name"] not in ("one-bank", "channel-8"):
        for write in ("0", "1"):
            cycles = sorted({int(beat["cycle"]) for request in full if request["write"] == write for beat in service[request["id"]]})
            assert len(cycles) == total // 32
            if vlenb == total == 64: assert cycles[1] == cycles[0] + 1

    # Independent peer accesses observe the shared bytes, not a QEMU shadow.
    ready_write, = (row for row in accepted.values() if row["requestor"].endswith(":qemu_memory")
                    and row["write"] == "1" and int(row["address"]) == 0x100040)
    ready_cycle = int(completed[ready_write["id"]]["cycle"])
    polls = [row for row in accepted.values() if row["requestor"].startswith("peer")
             and row["write"] == "0" and int(row["address"]) == 0x100040]
    observed = {int(row["index"]): int(row["value"]) for row in rows(trial / "peer-polls.csv")}
    checked = 0
    for index, poll in enumerate(polls):
        if int(poll["cycle"]) >= ready_cycle:
            assert observed[index] == 0xc001c0de
            checked += 1
    assert checked > 0
    vector_polls = rows(trial / "peer-vector-polls.csv")
    vector_reads = [row for row in accepted.values() if row["requestor"].startswith("peer")
                    and row["write"] == "0" and int(row["address"]) == 0x100044][:len(vector_polls)]
    assert len(vector_reads) == len(vector_polls)
    vector_write, = (row for row in accepted.values() if row["requestor"].endswith(":qemu_memory")
                    and row["write"] == "1" and int(row["address"]) == 0x100044)
    write_service = min(int(row["cycle"]) for row in service[vector_write["id"]])
    write_completed = int(completed[vector_write["id"]]["cycle"])
    observed_vector = {int(row["index"]): int(row["value"]) for row in vector_polls}
    old_reads, new_reads = 0, 0
    for index, request in enumerate(vector_reads):
        cycles = [int(row["cycle"]) for row in service[request["id"]]]
        if max(cycles) < write_service:
            assert observed_vector[index] == 0, "vector output became visible before its ordered bank service"
            old_reads += 1
        elif min(cycles) > write_completed:
            assert observed_vector[index] == 0x111, "queued peer read did not observe committed vector output"
            new_reads += 1
    assert old_reads and new_reads, (old_reads, new_reads)
    return dict(case=case["name"], cpu=cpu, spm=spm, peer=peer, outputs_checked_bytes=10 * initialized,
                fault_prefixes_checked=3, vector_code_patch_checked=True,
                scalar_request_bytes=4, full_vector_request_bytes=32,
                full_vector_service_cycles_per_request=expected_cycles,
                full_vector_phase_cycles=phases[1][1] - phases[1][0], ordered_peer_reads=checked,
                vector_peer_old_reads=old_reads, vector_peer_new_reads=new_reads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path, default=ROOT / "install/llvm/bin/clang")
    parser.add_argument("--qemu", type=Path, default=ROOT / "build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--build-info", type=Path, help="Reuse a matching build that includes RiscvPeer")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case["name"] for case in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [case for case in selected if case["name"] in args.case]
    output = (args.output or ROOT / "tests/results/source-new-vector-memory" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(output, flush=True)
    if args.build_info:
        tools = json.loads(args.build_info.read_text())
        assert str(SOURCE / "tests/riscv-qemu/peer.cc") in tools["command"], "Build must include RiscvPeer"
        for path, digest in tools["source_sha256"].items():
            assert sha256(path) == digest, f"Source changed: {path}; rebuild before reusing build.json"
    else:
        tools = build(output / "build", extra_sources=[SOURCE / "tests/riscv-qemu/peer.cc"])
    guests = {key: compile_guest(output, args.compiler.resolve(), *key)
              for key in sorted({(case["sew"], case["lmul"]) for case in selected})}
    (output / "metadata.json").write_text(json.dumps(dict(build=tools, guests=list(guests.values()),
        qemu=str(args.qemu.resolve()), qemu_sha256=sha256(args.qemu.resolve()),
        sources={str(path): sha256(path) for path in HERE.iterdir() if path.is_file()}), indent=2) + "\n")
    results = []
    for entry in selected:
        trial = output / entry["name"]; trial.mkdir()
        case = entry | dict(qemu=str(args.qemu.resolve()), elf=guests[entry["sew"], entry["lmul"]]["elf"],
            parameters=resolve(dict(riscv_vector_length_bits=entry["vlen"]) | entry.get("parameters", {})))
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        start = time.perf_counter()
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen([tools["sst"], "--num-threads=1", f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")],
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
    if "vlen256-e32-m1" in by_name:
        for name in ("one-bank", "channel-8"):
            if name in by_name:
                assert by_name[name]["full_vector_phase_cycles"] > by_name["vlen256-e32-m1"]["full_vector_phase_cycles"]
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS {len(results)} vector-memory cases", flush=True)


if __name__ == "__main__": main()
