"""Check exact byte-range SPM ordering and delayed external functional commits."""
import argparse
from collections import Counter
import csv
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
from configuration import resolve


def cases():
    names = ["disjoint", "disjoint-peer"]
    names += [f"{hazard}-{shape}" for hazard in ("raw", "war", "waw") for shape in ("exact", "partial")]
    names += ["older-waiter", "wide-separate", "wide-batch"]
    names += [f"invalid-{kind}" for kind in ("empty", "duplicate", "premature", "size", "foreign")]
    return [dict(name=name, negative=name.startswith("invalid-"), parameters=resolve(dict(
        spm_capacity_bytes=4096, spm_banks=1 if name == "invalid-premature" else 8))) for name in names]


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, name):
    values = [json.loads(line[len(name) + 1:]) for line in log.splitlines() if line.startswith(name + " ")]
    assert len(values) == 1, (name, values)
    return values[0]


def validate(trial, case, log, returncode):
    trace = rows(trial / "protocol.csv")
    protocol = {}
    for row in trace:
        label, event = row["label"], row["event"]
        item = {key: int(value) for key, value in row.items() if key not in ("label", "event")}
        if event == "issue":
            assert label not in protocol, (row, protocol)
            protocol[label] = dict(item, issue=item["cycle"])
        else:
            entry = protocol[label]
            assert all(entry[key] == item[key] for key in ("address", "bytes", "write", "external")), (row, entry)
            assert event not in entry and item["cycle"] >= entry["issue"], (row, entry)
            entry[event] = item["cycle"]
    if case["negative"]:
        assert returncode != 0 and "External SPM commit" in log, ("Expected controller rejection", returncode, log[-4000:])
        assert "invalid-commit" in protocol["A"], protocol
        assert (trial / "scratchpad.bin").read_bytes() == b"\x11" * 4096, "Rejected commit admitted a queued peer write"
        if "P" in protocol:
            assert "response" not in protocol["P"], ("Invalid batch partially released its ranges", protocol)
        if case["name"] == "invalid-premature":
            assert "response" not in protocol["A"], protocol
        return dict(case=case["name"], passed=True, expected_rejection=True, unchanged_bytes_checked=4096)
    assert returncode == 0, (case["name"], returncode, log[-4000:])
    report, spm = stats(log, "RANGE_ORDERING_RESULT"), stats(log, "SPM_STATS")
    assert report["passed"] and report["requests"] == len(protocol) and report["checked_bytes"] == 4096, report
    for label, entry in protocol.items():
        assert entry["response"] > entry["issue"], (label, entry)
        if entry["external"]:
            assert entry["response"] <= entry["functional"] == entry["commit"], (label, entry)
    assert protocol["A"]["commit"] >= protocol["A"]["response"] + 20, protocol
    signatures = {(entry["address"], entry["bytes"], entry["write"], bool(entry["external"])): label
                  for label, entry in protocol.items()}
    assert len(signatures) == len(protocol), protocol
    requests, by_label = {}, {}
    counts, byte_counts, service_counts = Counter(), Counter(), Counter()
    ports, channel_bytes, channel_ids = set(), Counter(), {}
    last_service = -1
    backend_paths = list(trial.glob("scratchpad*.csv"))
    assert len(backend_paths) == 1, backend_paths
    for row in rows(backend_paths[0]):
        event, identity = row["event"], row["id"]
        cycle, address, count, write = (int(row[key]) for key in ("cycle", "address", "bytes", "write"))
        if event == "accepted":
            assert identity not in requests, row
            key = address, count, write, row["requestor"] == "driver:external"
            label = signatures[key]
            assert label not in by_label and cycle >= protocol[label]["issue"], (row, label, protocol)
            request = dict(label=label, address=address, bytes=count, write=write, accepted=cycle, issued=0, services=[])
            requests[identity] = by_label[label] = request
            counts[event] += 1; byte_counts[write] += count
        else:
            assert identity in requests, (row, requests)
            request = requests[identity]
            assert request["write"] == write, (row, request)
            if event == "service":
                if cycle != last_service:
                    assert cycle > last_service, row
                    last_service, ports, channel_bytes, channel_ids = cycle, set(), Counter(), {}
                bank, port, channel = (int(row[key]) for key in ("bank", "port", "channel"))
                p = case["parameters"]
                assert address == request["address"] + request["issued"], (row, request)
                assert bank == address // p["spm_bank_width"] % p["spm_banks"], row
                assert 0 < count <= min(request["bytes"] - request["issued"], p["spm_bank_width"] - address % p["spm_bank_width"]), (row, request)
                limit = p["spm_write_ports_per_bank"] if write else p["spm_read_ports_per_bank"]
                assert 0 <= port < limit and (bank, port, write) not in ports, ("Reused bank port", row)
                ports.add((bank, port, write))
                assert 0 <= channel < p["spm_channels"] and channel_ids.setdefault(channel, identity) == identity, ("Reused channel", row)
                channel_bytes[channel] += count
                assert channel_bytes[channel] <= p["spm_channel_width"], row
                request["issued"] += count
                request["services"].append(cycle)
                service_counts[write] += 1
            else:
                assert event == "completed" and count == request["bytes"] == request["issued"] and address == request["address"], (row, request)
                assert cycle == max(request["services"]) + 1 <= protocol[request["label"]]["response"], (row, request)
                request["completed"] = cycle
                counts[event] += 1
                del requests[identity]
    assert not requests and set(by_label) == set(protocol), (requests, by_label, protocol)
    assert counts["accepted"] == counts["completed"] == spm["accepted"] == spm["completed"] == len(protocol), (counts, spm)
    assert byte_counts[0] == spm["read_bytes"] and byte_counts[1] == spm["write_bytes"], (byte_counts, spm)
    assert service_counts[0] == spm["read_service_beats"] and service_counts[1] == spm["write_service_beats"], (service_counts, spm)
    name = case["name"]
    if name in ("disjoint", "disjoint-peer"):
        assert by_label["A"]["accepted"] == by_label["B"]["accepted"], by_label
        assert set(by_label["A"]["services"]) == set(by_label["B"]["services"]), by_label
        assert len(set(by_label["A"]["services"])) == 1, by_label
        assert protocol["B"]["response"] < protocol["A"]["commit"], protocol
    elif name.startswith(("raw-", "war-", "waw-")):
        assert protocol["P"]["issue"] < protocol["A"]["commit"] <= by_label["P"]["accepted"], (protocol, by_label)
    elif name == "older-waiter":
        assert protocol["P"]["issue"] < protocol["B"]["issue"] < protocol["A"]["commit"], protocol
        assert by_label["P"]["accepted"] >= protocol["A"]["commit"], (protocol, by_label)
        assert by_label["B"]["accepted"] >= by_label["P"]["completed"], ("Younger request bypassed queued overlap", by_label)
    else:
        assert name in ("wide-separate", "wide-batch"), name
        for peer, external in (("P", "A"), ("Q", "B")):
            assert protocol[peer]["issue"] < protocol[external]["commit"] <= by_label[peer]["accepted"], (protocol, by_label)
        if name == "wide-separate":
            assert protocol["P"]["response"] < protocol["B"]["commit"], protocol
        else:
            assert protocol["A"]["commit"] == protocol["B"]["commit"], protocol
    return dict(case=name, passed=True, requests=len(protocol), checked_bytes=4096, protocol=protocol,
                backend=by_label, stats=spm)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-info", type=Path, default=ROOT / "build/src/components/build.json")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case["name"] for case in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [case for case in selected if case["name"] in args.case]
    build = json.loads(args.build_info.read_text())
    assert str(HERE / "protocol.cc") in build["command"], "Shared build must include range-ordering/protocol.cc"
    for path, expected in build["source_sha256"].items():
        assert digest(path) == expected, ("Source changed since build", path)
    output = (args.output or ROOT / "tests/results/source-new-range-ordering" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(output, flush=True)
    (output / "metadata.json").write_text(json.dumps(dict(build=build, sources={str(p): digest(p) for p in HERE.iterdir() if p.is_file()}), indent=2) + "\n")
    results = []
    for case in selected:
        trial = output / case["name"]; trial.mkdir()
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        with (trial / "simulation.log").open("w") as log:
            process = subprocess.Popen([build["sst"], "--num-threads=1", f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")],
                env=os.environ | dict(SST_LIB_PATH=build["plugin"] + ":" + build["library"], TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try: process.wait(timeout=30)
            finally:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.wait()
        result = validate(trial, case, (trial / "simulation.log").read_text(), process.returncode)
        results.append(result)
        (trial / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print("PASS", case["name"], flush=True)
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results)), indent=2) + "\n")
    print(f"PASS {len(results)} range-ordering regressions", flush=True)


if __name__ == "__main__":
    main()
