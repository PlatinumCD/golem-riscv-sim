"""Check bounded vector LSQ overlap, dependencies, retirement and drain points."""
import argparse
from collections import Counter, deque
import csv
import hashlib
import importlib.util
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
from configuration import resolve
import grouped


def cases():
    result = [dict(name=f"depth-{depth}", depth=depth, vlen=256, banks=1, mode=0)
              for depth in (1, 2, 4, 8, 16)]
    result += [dict(name=f"vlen512-depth-{depth}", depth=depth, vlen=512, banks=8, mode=0)
               for depth in (1, 4)]
    result += [dict(name=name, depth=4, vlen=256, banks=1, mode=mode)
               for name, mode in (("fence", 1), ("fence-i", 2), ("exit", 3), ("fault", 4), ("analog", 5))]
    result += [dict(name="repeat-depth-4", depth=4, vlen=256, banks=1, mode=0),
               dict(name="budget-1", depth=4, vlen=256, banks=1, mode=0, options=dict(instruction_budget=1)),
               dict(name="issue-4", depth=4, vlen=256, banks=1, mode=0, options=dict(issue_width=4)),
               dict(name="vector-fallback", depth=4, vlen=256, banks=8, mode=0, fallback=True)]
    result += [dict(name=f"partial-vlen{vlen}-depth-{depth}", depth=depth,
                    vlen=vlen, banks=1, mode=6, peer=False)
               for vlen in (128, 1024) for depth in (1, 4)]
    result += [dict(name="instruction-fetch-fault", depth=4, vlen=256, banks=1, mode=7)]
    result += [dict(name=f"grouped-vlen{vlen}-depth-{depth}", depth=depth,
                    vlen=vlen, banks=1, mode=8, peer=False)
               for vlen in (128, 1024) for depth in (1, 4)]
    return result


def digest(path):
    with Path(path).open("rb") as stream: return hashlib.file_digest(stream, "sha256").hexdigest()


def rows(path):
    with path.open() as stream: return list(csv.DictReader(stream))


def stats(log, label):
    found = [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]
    assert len(found) == 1, (label, found)
    return found[0]


def compile_guest(output, compiler, vlen, mode, fallback):
    elf = output / f"guest-{vlen}-{mode}-{int(fallback)}.elf"
    support = HERE.parent / "riscv-qemu"
    files = ([HERE.parent / "single-tile-runtime/measurement.S", HERE.parent / "vector-memory/trap.S",
              HERE.parent / "vector-memory/guest.c"] if fallback else
             [HERE.parent / "array-pipeline/measurement.S", HERE / "trap.S", HERE / "kernels.S", HERE / "guest.c"])
    if mode == 6:
        files = [HERE.parent / "array-pipeline/measurement.S", HERE / "partial.c"]
    if mode == 8:
        files = [HERE.parent / "array-pipeline/measurement.S", HERE / "grouped.c"]
    command = [str(compiler), "--target=riscv64-unknown-elf", "-mcpu=golem-analog", "-fuse-ld=lld",
        "-mabi=lp64d", "-mcmodel=medany", "-msmall-data-limit=0", "-O1", "-fno-vectorize", "-fno-slp-vectorize",
        "-ffreestanding", "-fno-builtin", "-nostdlib", "-static", "-Wl,--no-relax", "-Wl,--build-id=none",
        f"-DVLEN_BITS={vlen}", f"-DMODE={mode}", "-T", str(support / "scratchpad.ld"),
        str(support / "start.S"), *map(str, files), "-o", str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    elf.with_suffix(".build.log").write_text(process.stdout + process.stderr)
    if process.returncode: raise RuntimeError(process.stderr)
    disassembly = subprocess.check_output([str(compiler.parent / "llvm-objdump"), "-d", str(elf)], text=True)
    elf.with_suffix(".asm").write_text(disassembly)
    symbols = {}
    for line in subprocess.check_output([str(compiler.parent / "llvm-nm"), "--defined-only", str(elf)], text=True).splitlines():
        columns = line.split()
        if len(columns) == 3 and columns[2] in ("fetch_fault_jump", "trap_handler"):
            symbols[columns[2]] = int(columns[0], 16)
    return dict(elf=str(elf), sha256=digest(elf), command=command, symbols=symbols)


def check_lsq(trial, case, cpu):
    trace = rows(trial / "riscv-lsq.csv")
    active, entries, order = {}, {}, deque()
    stalls = Counter()
    peak = 0
    previous_cycle = -1
    for line, row in enumerate(trace, 2):
        event, reason = row["event"], row["reason"]
        data = {key: int(value) for key, value in row.items() if key not in ("event", "reason")}
        token, cycle = data["token"], data["cycle"]
        assert cycle >= previous_cycle, (line, row)
        previous_cycle = cycle
        if event == "stall":
            assert reason in ("register", "full", "drain"), (line, row)
            assert token == data["slot"] == data["address"] == data["bytes"] == data["write"] == 0, (line, row)
            stalls[reason] += 1
        elif event == "enqueue":
            assert token not in entries and token not in active, (line, row)
            assert 0 <= data["slot"] < case["depth"], (line, row)
            assert data["slot"] not in {entry["slot"] for entry in active.values()}, (line, row)
            assert 0 < data["bytes"] <= case["vlen"] // 8 and data["write"] in (0, 1), (line, row)
            entries[token] = active[token] = data | dict(enqueue=cycle)
            order.append(token)
            peak = max(peak, len(active))
        else:
            assert token in active and event in ("issue", "service_complete", "complete"), (line, row)
            entry = active[token]
            assert all(entry[key] == data[key] for key in ("slot", "pc", "address", "bytes", "write")), (line, row, entry)
            assert event not in entry, (line, row)
            entry[event] = cycle
            if event == "issue": assert cycle == entry["enqueue"], (line, row, entry)
            elif event == "service_complete": assert cycle > entry["issue"], (line, row, entry)
            else:
                assert cycle >= entry["service_complete"] and order.popleft() == token, (line, row, entry)
                del active[token]
        assert data["occupancy"] == len(active), ("LSQ occupancy disagrees with lifecycle", line, row, len(active))
        assert len(active) <= case["depth"], (line, row)
    assert not active and not order, active
    assert len(entries) == cpu["lsq_enqueued"] == cpu["lsq_completed"], (len(entries), cpu)
    assert peak == cpu["lsq_peak_occupancy"] <= case["depth"], (peak, cpu)
    assert cpu["load_store_queue_depth"] == case["depth"], cpu
    for reason in ("register", "full", "drain"):
        assert stalls[reason] == cpu[f"lsq_{reason}_stalls"], (reason, stalls, cpu)
    assert 0 <= cpu["lsq_stall_cycles"] <= cpu["end_cycle"], cpu
    if case["depth"] == 1:
        assert not trace and not entries and cpu["lsq_stall_cycles"] == 0, (trace, cpu)
    else:
        assert entries and stalls["drain"] > 0, (peak, stalls)
        if not case.get("fallback") and case["mode"] != 5:
            assert peak > 1, ("No independent LSQ overlap", peak, stalls)
    return list(entries.values()), trace, stalls


def check_peer(trial, case, entries):
    trace = rows(next(trial.glob("scratchpad*.csv")))
    accepted, completed = {}, {}
    for row in trace:
        if row["event"] == "accepted": accepted[row["id"]] = row
        elif row["event"] == "completed": completed[row["id"]] = row
    vector_poll_rows = rows(trial / "peer-vector-polls.csv")
    polls = [row for row in accepted.values() if row["requestor"].startswith("peer") and row["write"] == "0"
             and int(row["address"]) == 0x100044][:len(vector_poll_rows)]
    values = {int(row["index"]): int(row["value"]) for row in vector_poll_rows}
    writes = [entry for entry in entries if entry["write"] and entry["address"] == 0x90100044]
    if not writes: return dict(vector_polls_checked=0)
    assert len(writes) == 1 and writes[0]["bytes"] == 32, writes
    commit = writes[0]["complete"]
    before = after = 0
    for index, poll in enumerate(polls):
        done = int(completed[poll["id"]]["cycle"])
        if done < commit:
            assert values[index] == 0, ("Peer observed store before LSQ commit", poll, values[index], commit)
            before += 1
        elif int(poll["cycle"]) > commit:
            assert values[index] == 0x111, ("Peer did not observe committed vector store", poll, values[index], commit)
            after += 1
    assert before > 0 and after > 0, (before, after)
    return dict(vector_polls_checked=before+after, peer_reads_before_commit=before, peer_reads_after_commit=after)


def check_outputs(trial, case):
    if case["mode"] == 8:
        return grouped.check_outputs(trial, case)
    lanes = case["vlen"] // 32
    def expected(start, count): return list(range(0x1000+start, 0x1000+start+count))
    with (trial / "scratchpad.bin").open("rb") as memory:
        def words(slot, count):
            memory.seek(0x110000+slot*0x1000)
            return list(struct.unpack(f"<{count}I", memory.read(count*4)))
        if case["mode"] == 5:
            memory.seek(0x110000+30*0x1000)
            assert list(struct.unpack("<32f", memory.read(128))) == [i%7-3 for i in range(32)]
            return 32
        if case["mode"] == 6:
            vector_bytes = case["vlen"] // 8
            checked = 0
            for phase in range(8):
                element_bytes = 1 << (phase // 2)
                for register in range(2):
                    prefix = bytes((13 * i + 7) & 255 for i in
                                   range(register * vector_bytes, (register + 1) * vector_bytes - element_bytes))
                    tail = (3 + 2 * register).to_bytes(element_bytes, "little")
                    memory.seek(0x160000 + phase * 0x1000 + register * vector_bytes)
                    actual = memory.read(vector_bytes)
                    assert actual[:len(prefix)] == prefix, ("Partial load active bytes", phase, register, actual.hex(), prefix.hex())
                    # RISC-V permits either the old element or all ones for TA.
                    # The depth-1/4 comparison below additionally requires the
                    # asynchronous implementation to match this QEMU's policy.
                    allowed = (tail, b"\xff" * element_bytes) if phase % 2 else (tail,)
                    assert actual[len(prefix):] in allowed, ("Load tail policy", phase, register, actual.hex(), allowed)
                    memory.seek(0x170000 + phase * 0x1000 + register * vector_bytes)
                    actual = memory.read(vector_bytes)
                    wanted = prefix + b"\xa5" * element_bytes
                    assert actual == wanted, ("Short store changed its tail", phase, register, actual.hex(), wanted.hex())
                    checked += 2 * vector_bytes
            return checked
        if case["mode"] == 7:
            assert words(20, lanes) == expected(lanes, lanes), "Older store lost on fetch fault"
            memory.seek(0x150000)
            assert list(struct.unpack(f"<{lanes}I", memory.read(lanes * 4))) == expected(0, lanes), "Trap saw stale loaded register"
            memory.seek(0x150080)
            assert struct.unpack("<I", memory.read(4))[0] == 0x1000 + lanes, "Trap saw stale store"
            memory.seek(0x140000)
            assert struct.unpack("<4Q", memory.read(32)) == (1, 0x90200000, 0, 1), "Unexpected fetch-fault record"
            return 2 * lanes + 1
        if case["mode"]:
            assert words(20, 24*lanes) == expected(0, 24*lanes), "Drain lost pending store data"
            if case["mode"] == 4:
                memory.seek(0x140000)
                assert struct.unpack("<4Q", memory.read(32)) == (5, 0x90200000, 0, 1)
            return 24*lanes
        checks = {1:expected(0,24*lanes),2:expected(0,24*lanes),
            3:expected(0,lanes)+expected(1,lanes),4:expected(lanes,lanes),
            5:expected(0,2*lanes),6:expected(0,lanes),
            7:expected(1,lanes-1)+[0x9000+lanes],8:expected(0,1)+expected(lanes,lanes),
            9:list(range(0x9000,0x9000+lanes)),10:expected(0,lanes)+[7]*lanes}
        for slot, wanted in checks.items():
            actual = words(slot,len(wanted))
            assert actual == wanted, ("Guest result mismatch", slot, actual, wanted)
        return sum(map(len,checks.values()))


def validate(trial, case, log):
    cpu, spm = (stats(log, key) for key in ("RISCV_STATS", "SPM_STATS"))
    if case.get("peer", True):
        peer = stats(log, "RISCV_PEER_RESULT")
        assert peer["passed"] and peer["verified_bytes"] == 40
    assert cpu["memory_requests"] == cpu["completed_requests"] and spm["accepted"] == spm["completed"]
    entries, trace, stalls = check_lsq(trial, case, cpu)
    topology = json.loads((trial / "topology.json").read_text())
    node, = (node for node in topology["components"] if node["type"] == "tilecomponents.RiscvQemu")
    assert int(node["params"]["load_store_queue_depth"]) == case["depth"]
    markers = rows(trial / "riscv-tasks.csv")
    for row in markers:
        assert int(row["memory_requests"]) == int(row["completed_requests"]), row
        assert int(row["lsq_enqueued"]) == int(row["lsq_completed"]), row
    phases = {}
    if not case.get("fallback"):
        expected_ids = (grouped.PHASE_IDS if case["mode"] == 8 else list(range(1,11)) if case["mode"] == 0 else list(range(40,48)) if case["mode"] == 6
                        else [30 if case["mode"] == 5 else 21 if case["mode"] == 7 else 20])
        assert len(markers) == len(expected_ids)*2 - (case["mode"] == 3), markers
        for index, identity in enumerate(expected_ids):
            start = markers[index*2]
            assert start["event"] == "start" and int(start["task_id"]) == identity, start
            if index*2+1 >= len(markers):
                finish_cycle = cpu["end_cycle"]
            else:
                finish = markers[index*2+1]
                assert finish["event"] == "finish" and int(finish["task_id"]) == identity, finish
                finish_cycle = int(finish["cycle"])
            begin = int(start["cycle"])
            selected = [entry for entry in entries if begin <= entry["enqueue"] <= finish_cycle]
            assert all(entry["complete"] <= finish_cycle for entry in selected), (identity, selected)
            occupancy = peak = 0
            tokens = {entry["token"] for entry in selected}
            for row in trace:
                if int(row["token"]) in tokens:
                    occupancy += (row["event"] == "enqueue") - (row["event"] == "complete")
                    peak = max(peak,occupancy)
            phase_stalls = Counter(row["reason"] for row in trace if row["event"] == "stall"
                                   and begin <= int(row["cycle"]) <= finish_cycle)
            phases[str(identity)] = dict(cycles=finish_cycle-begin, entries=len(selected), peak=peak,
                reads=sum(not entry["write"] for entry in selected),writes=sum(entry["write"] for entry in selected),
                stalls=dict(phase_stalls))
            if case["mode"] == 8:
                grouped.check_phase(identity, selected, case, phases[str(identity)])
            if case["mode"] == 6 and case["depth"] > 1:
                phase = identity - 40
                element_bytes = 1 << (phase // 2)
                vector_bytes = case["vlen"] // 8
                expected = [(0x90101000, 0), (0x90101000 + vector_bytes, 0),
                            (0x90170000 + phase * 0x1000, 1), (0x90170000 + phase * 0x1000 + vector_bytes, 1)]
                actual = [(entry["address"], entry["write"]) for entry in selected]
                assert actual == expected, ("Partial transfer did not use four async entries", identity, actual, expected)
                assert all(entry["bytes"] == vector_bytes - element_bytes for entry in selected), (identity, selected)
                assert peak >= 2, ("Independent short loads did not overlap", identity, selected)
        if case["depth"] > 1 and case["mode"] == 0:
            assert phases["1"]["entries"] == phases["2"]["entries"] == 24, phases
            assert phases["1"]["peak"] > 1 and phases["2"]["peak"] > 1, phases
            assert phases["5"]["peak"] > 1, ("Store snapshot prevented a safe register overwrite",phases)
            assert phases["10"]["peak"] > 1, ("Vector ALU unnecessarily waited for a captured store",phases)
            assert stalls["register"] > 0, stalls
            assert all(phases[phase]["stalls"].get("register",0)>0 for phase in ("3","4")), phases
            if case["banks"] == 1:
                assert phases["1"]["peak"] == phases["2"]["peak"] == case["depth"], phases
                assert stalls["full"] > 0, stalls
        checked = check_outputs(trial,case)
        if case["mode"] == 7:
            selected = [entry for entry in entries if entry["address"] in (0x90101000, 0x90124000)
                        and int(markers[0]["cycle"]) <= entry["enqueue"] <= int(markers[1]["cycle"])]
            assert [(entry["address"], entry["write"]) for entry in selected] == [(0x90101000, 0), (0x90124000, 1)], selected
            fetches = rows(trial / "riscv-icache.csv")
            def lookup_cycle(symbol):
                matches = [int(row["cycle"]) for row in fetches if row["event"] in ("hit", "miss")
                           and int(row["address"]) == case["symbols"][symbol]]
                assert len(matches) == 1, (symbol, matches)
                return matches[0]
            jump, handler = lookup_cycle("fetch_fault_jump"), lookup_cycle("trap_handler")
            # Admission can resume QEMU and fetch its next instruction in the
            # same cycle. Both accesses must nevertheless remain outstanding
            # at the jump lookup and retire before the first handler lookup.
            assert all(entry["pc"] < case["symbols"]["fetch_fault_jump"] and
                       entry["enqueue"] <= jump < entry["complete"] <= handler for entry in selected), (
                "Fault must occur with both operations pending, and retire them before handler fetch", jump, handler, selected)
            assert phases["21"]["stalls"].get("drain", 0) > 0, phases
    else:
        path = HERE.parent / "vector-memory/run.py"
        spec = importlib.util.spec_from_file_location("vector_memory_lsq_reference",path)
        module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        original = case | dict(name="vlen256-e32-m1",sew=32,lmul="m1")
        module.validate(trial,original,log)
        checked = 0
    if case["mode"] == 5:
        arrays = stats(log,"ARRAY_STATS")
        assert arrays["errors"] == arrays["busy"] == 0 and arrays["mvms"] == 1,arrays
        assert arrays["accepted"] == arrays["completed"] == cpu["analog_commands"]
    else:
        assert cpu["analog_commands"] == 0
    observed = check_peer(trial,case,entries) if case.get("peer", True) else dict(vector_polls_checked=0)
    return dict(case=case["name"],cpu=cpu,spm=spm,phases=phases,outputs_checked=checked,
                trace_sha256=digest(trial/'riscv-lsq.csv'),**observed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case",action="append")
    parser.add_argument("--output",type=Path)
    parser.add_argument("--compiler",type=Path,default=ROOT/"install/llvm/bin/clang")
    parser.add_argument("--qemu",type=Path,default=ROOT/"build/src/qemu/qemu-system-riscv64")
    parser.add_argument("--build-info",type=Path,default=ROOT/"build/src/components/build.json")
    args=parser.parse_args()
    selected=cases()
    if args.case:
        unknown=set(args.case)-{case['name'] for case in selected}
        if unknown:parser.error(f"Unknown cases: {sorted(unknown)}")
        selected=[case for case in selected if case['name'] in args.case]
    build=json.loads(args.build_info.read_text())
    assert str(SOURCE/'tests/riscv-qemu/peer.cc') in build['command'],"Build must include RiscvPeer"
    for path,sha in build['source_sha256'].items():assert digest(path)==sha,("Source changed after build",path)
    output=(args.output or ROOT/'tests/results/source-new-load-store-queue'/str(time.time_ns())).resolve()
    output.mkdir(parents=True,exist_ok=False)
    print(output,flush=True)
    keys={(case['vlen'],case['mode'],case.get('fallback',False)) for case in selected}
    guests={key:compile_guest(output,args.compiler.resolve(),*key) for key in sorted(keys)}
    (output/'metadata.json').write_text(json.dumps(dict(build=build,qemu_sha256=digest(args.qemu.resolve()),
        guests=list(guests.values()),sources={str(p):digest(p) for p in HERE.iterdir() if p.is_file()}),indent=2)+'\n')
    results=[]
    for entry in selected:
        trial=output/entry['name'];trial.mkdir()
        guest=guests[entry['vlen'],entry['mode'],entry.get('fallback',False)]
        case=entry|dict(elf=guest['elf'],symbols=guest['symbols'],qemu=str(args.qemu.resolve()),
            parameters=resolve(dict(riscv_vector_length_bits=entry['vlen'],spm_banks=entry['banks'])),
            cpu_parameters=dict(load_store_queue_depth=entry['depth'])|entry.get('options',{}))
        (trial/'case.json').write_text(json.dumps(case,indent=2)+'\n')
        with (trial/'simulation.log').open('w') as log:
            process=subprocess.Popen([build['sst'],'--num-threads=1',f"--output-json={trial/'topology.json'}",str(HERE/'simulation.py')],
                env=os.environ|dict(SST_LIB_PATH=build['plugin']+':'+build['library'],TILE_COMPONENT_OUTPUT=str(trial),PYTHONDONTWRITEBYTECODE='1'),
                stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:process.wait(timeout=120)
            finally:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
                process.wait()
        log=(trial/'simulation.log').read_text()
        if process.returncode:raise RuntimeError(f"{entry['name']} failed: {log[-8000:]}")
        result=validate(trial,case,log);results.append(result)
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('PASS',entry['name'],flush=True)
    by_name={result['case']:result for result in results}
    if 'depth-4' in by_name:
        reference=by_name['depth-4']
        for name in ('repeat-depth-4','budget-1'):
            if name in by_name:
                assert by_name[name]['phases']==reference['phases'],(name,'phase timing changed')
                assert by_name[name]['trace_sha256']==reference['trace_sha256'],(name,'nondeterministic LSQ trace')
        if 'depth-1' in by_name:
            for phase in ('1','2'):
                assert reference['phases'][phase]['cycles']<by_name['depth-1']['phases'][phase]['cycles']
    for vlen in (128, 1024):
        names = [f'partial-vlen{vlen}-depth-{depth}' for depth in (1, 4)]
        if all(name in by_name for name in names):
            snapshots = []
            for name in names:
                with (output/name/'scratchpad.bin').open('rb') as memory:
                    data = []
                    for phase in range(8):
                        memory.seek(0x160000 + phase * 0x1000)
                        data.append(memory.read(vlen // 4))
                    snapshots.append(data)
            assert snapshots[0] == snapshots[1], ('Async tail behavior differs from synchronous QEMU', vlen)
    for vlen in (128, 1024):
        names = [f'grouped-vlen{vlen}-depth-{depth}' for depth in (1, 4)]
        if all(name in by_name for name in names):
            assert grouped.snapshots(output / names[0], vlen) == grouped.snapshots(output / names[1], vlen), (
                'Grouped tail behavior differs from synchronous QEMU', vlen)
    (output/'validation.json').write_text(json.dumps(dict(passed=True,cases=len(results)),indent=2)+'\n')
    print(f'PASS {len(results)} LSQ regressions',flush=True)


if __name__=='__main__':main()
