"""Build and validate real 2x2 Mordred routing, payloads and transmit backpressure."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from components.mordred.build import build
from validate import validate_trial


def cases():
    return [
        dict(name="all-to-all", mesh={}, endpoint=dict(num_messages=16, message_size=64)),
        dict(name="minimum-packet", mesh={}, endpoint=dict(num_messages=8, message_size=32)),
        dict(name="partial-flit", mesh={}, endpoint=dict(num_messages=8, message_size=33)),
        dict(name="large-packet", mesh={}, endpoint=dict(num_messages=32, message_size=1024)),
        dict(name="tight-credits", mesh=dict(router_input_buffer_flits=1,
            router_output_buffer_flits=1, nic_input_buffer_bytes=32, nic_output_buffer_bytes=256),
            endpoint=dict(num_messages=64, message_size=256)),
        dict(name="two-vcs", mesh=dict(num_vcs=2), endpoint=dict(num_messages=16, message_size=64)),
        dict(name="slower-links", mesh=dict(link_latency="3ns"),
            endpoint=dict(num_messages=16, message_size=64)),
    ]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/source_new-mordred-tests")
    args = parser.parse_args()
    output = (args.output or ROOT / "tests/results/source-new-mordred" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(output, flush=True)
    build_dir = args.build_dir.resolve()
    network = build(build_dir)
    core = Path(network["sst"]).parents[1]
    config = core / "bin/sst-config"
    compiler = shlex.split(subprocess.check_output([str(config), "--CXX"], text=True))
    flags = shlex.split(subprocess.check_output([str(config), "--ELEMENT_CXXFLAGS"], text=True))
    inputs = sorted(p for directory in (HERE, SOURCE / "components/mordred")
                    for p in directory.rglob("*") if p.suffix in (".py", ".cc", ".h", ".json"))
    before = {str(p): sha(p) for p in inputs}
    command = compiler + flags + ["-O2", "-Wall", "-Wextra", "-shared",
        str(HERE / "meshEndpoint.cc"), "-o", str(build_dir / "libmordredtests.so")]
    with (build_dir / "endpoint-build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    library_hashes = {str(path): sha(path) for path in
                     (build_dir / "libmordredtests.so", build_dir / "libmordred.so")}
    results = []
    for case in cases():
        trial = output / case["name"]
        trial.mkdir()
        case_file = trial / "case.json"
        case_file.write_text(json.dumps(case, indent=2) + "\n")
        invocation = [network["sst"], "--num-threads=1", f"--add-lib-path={build_dir}",
                      f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")]
        environment = os.environ | dict(MORDRED_TEST_CASE=str(case_file), PYTHONDONTWRITEBYTECODE="1")
        process = subprocess.run(invocation, env=environment, capture_output=True, text=True, timeout=60)
        (trial / "simulation.log").write_text(process.stdout + process.stderr)
        process.check_returncode()
        records = [json.loads((trial / f"endpoint{i}.json").read_text()) for i in range(4)]
        # The endpoint independently checks every source/destination/sequence/VN
        # and payload byte before marking a receive complete.
        expected = 3 * case["endpoint"]["num_messages"]
        for record in records:
            assert record["passed"] is True, record
            assert record["sent"] == record["received"] == expected, record
        checks = validate_trial(trial, case, records)
        result = dict(name=case["name"], passed=True, packets=4 * expected,
                      payload_bytes=4 * expected * case["endpoint"]["message_size"], endpoints=records,
                      simulated_cycles=max(record["completion_cycle"] for record in records),
                      send_blocked_cycles=sum(record["send_blocked_cycles"] for record in records),
                      checks=checks, command=invocation)
        results.append(result)
        (trial / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"PASS {case['name']}: {result['packets']} packets, {result['simulated_cycles']} cycles", flush=True)
    baseline = next(result for result in results if result["name"] == "all-to-all")
    slower = next(result for result in results if result["name"] == "slower-links")
    for fast, slow in zip(baseline["endpoints"], slower["endpoints"]):
        # Compare the first delivery. Overall throughput under contention can
        # improve when link delays change arbitration phase and packet spacing.
        assert slow["first_arrival_tick"] > fast["first_arrival_tick"], (fast, slow)
    assert before == {str(p): sha(p) for p in inputs}, "Sources changed during validation"
    assert library_hashes == {path: sha(Path(path)) for path in library_hashes}, "Libraries changed during validation"
    summary = dict(passed=True, configurations=len(results), results=results,
                   packets=sum(result["packets"] for result in results),
                   payload_bytes=sum(result["payload_bytes"] for result in results),
                   longer_links_delay_first_delivery=True,
                   mordred_build=network, endpoint_build_command=command,
                   library_sha256=library_hashes, source_sha256=before)
    (output / "validation.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"PASS {len(results)} Mordred mesh configurations: {output / 'validation.json'}", flush=True)


if __name__ == "__main__":
    main()
