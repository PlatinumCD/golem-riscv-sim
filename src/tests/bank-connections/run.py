"""Check physical bank connectivity, shared storage, arbitration and rejection."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = json.loads(args.build_info.read_text())
    assert str(HERE / "driver.cc") in build["command"]
    for path, digest in build["source_sha256"].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest, ("stale build", path)
    args.output.mkdir(parents=True, exist_ok=False)
    cases = ["visibility", "disjoint", "shared-conflict", "shared-two-ports", "shared-read-write",
             "visibility-router-one", "visibility-router-three",
             "cpu-forbidden", "router-forbidden", "cpu-span", "router-span", "unknown-requestor"]
    results = []
    for name in cases:
        trial = args.output / name
        trial.mkdir()
        (trial / "case.json").write_text(json.dumps(dict(name=name)))
        start = time.monotonic()
        proc = subprocess.run([build["sst"], "--num-threads=1", str(HERE / "simulation.py")],
            env=os.environ | dict(SST_LIB_PATH=build["plugin"] + ":" + build["library"],
                                  TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        (trial / "simulation.log").write_text(proc.stdout)
        negative = name in cases[7:]
        expected = bytearray(b"\x11" * 4096)
        if negative:
            assert proc.returncode != 0 and "SPM bank connection denied" in proc.stdout, proc.stdout[-4000:]
            assert (trial / "spm.bin").read_bytes() == expected, "Denied request changed physical bytes"
            result = dict(case=name, passed=True, denied_before_mutation=True)
        else:
            assert proc.returncode == 0 and 'BANK_CONNECTION_RESULT {"passed":true' in proc.stdout, proc.stdout[-5000:]
            if name.startswith("visibility"): expected[8:12] = b"\x41" * 4
            elif name == "disjoint": expected[0:4], expected[4:8] = b"\x31" * 4, b"\x41" * 4
            else:
                if name != "shared-read-write": expected[8:12] = b"\x31" * 4
                expected[24:28] = b"\x41" * 4
            assert (trial / "spm.bin").read_bytes() == expected
            files = list(trial.glob("scratchpad*.csv"))
            assert len(files) == 1
            rows = list(csv.DictReader(files[0].open()))
            service = [row for row in rows if row["event"] == "service"]
            resources = set()
            by_role = defaultdict(list)
            connections = dict(cpu=[0, 2], router=[1, 2])
            if name == "visibility-router-one": connections = dict(cpu=[0, 1, 2, 3], router=[2])
            if name == "visibility-router-three": connections = dict(cpu=[2], router=[1, 2, 3])
            for row in service:
                role = row["requestor"].split(":")[-1]
                cycle, address, bank, port, write = [int(row[k]) for k in ("cycle", "address", "bank", "port", "write")]
                assert bank == address // 4 % 4 and bank in connections[role]
                key = cycle, bank, port, write
                assert key not in resources
                resources.add(key)
                assert port < (2 if name == "shared-two-ports" and write else 1)
                by_role[role].append(cycle)
            if name == "shared-conflict":
                assert abs(by_role["cpu"][0] - by_role["router"][0]) == 1
            elif name in ("disjoint", "shared-two-ports", "shared-read-write"):
                assert by_role["cpu"] == by_role["router"], (name, by_role)
            result = dict(case=name, passed=True, bank_service_cycles=dict(by_role), verified_bytes=len(expected))
        result["wall_seconds"] = time.monotonic() - start
        results.append(result)
        print(json.dumps(result), flush=True)
    (args.output / "validation.json").write_text(json.dumps(dict(passed=True, build=str(args.build_info), cases=results), indent=2) + "\n")


if __name__ == "__main__":
    main()
