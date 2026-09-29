"""Validate direct self-target bank accesses without any network packets."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def records(log, prefix):
    return [json.loads(line[len(prefix)+1:]) for line in log.splitlines() if line.startswith(prefix + " ")]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "tests/results/mordred-local" / str(time.time_ns()))
    args = parser.parse_args()
    info = json.loads(args.build_info.read_text())
    build = args.build_info.resolve().parent
    assert str(HERE / "driver.cc") in info["command"], "Build must include mordred-local/driver.cc"
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    paths = [build / "libtilecomponents.so", build / "libmordred.so", args.build_info]
    paths += list(HERE.glob("*.py")) + [HERE / "driver.cc"]
    hashes = {str(path.resolve()): sha(path) for path in paths}
    command = [info["sst"], "--num-threads=1", f"--add-lib-path={build}",
               f"--output-json={output / 'topology.json'}", str(HERE / "simulation.py")]
    environment = os.environ | dict(TILE_COMPONENT_OUTPUT=str(output), PYTHONDONTWRITEBYTECODE="1")
    environment.pop("TILE_COMPONENT_TRACE_START_TASK", None)
    environment.pop("TILE_COMPONENT_PROGRAM_PROOF", None)
    with (output / "simulation.log").open("w") as log:
        result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    log = (output / "simulation.log").read_text()
    assert result.returncode == 0, log[-6000:]
    assert len(records(log, "MORDRED_LOCAL_TEST")) == 1
    endpoint, = records(log, "MORDRED_SPM_STATS")
    assert endpoint["idle"] and endpoint["local_bank_completed"] == 12
    assert endpoint["local_bank_rejected"] == 4 and endpoint["local_rejected"] == 2
    assert endpoint["bytes_read"] == 64 and endpoint["bytes_written"] == 32
    assert endpoint["max_local_requests"] == 4 and endpoint["max_pending_memory"] <= 2
    for key in ("local_completed", "remote_completed", "remote_rejected", "requests_sent", "responses_sent",
                "requests_received", "responses_received", "wire_bytes_sent", "wire_bytes_received",
                "packets_injected", "max_pending_nic_packets", "max_incoming_requests"):
        assert endpoint[key] == 0, (key, endpoint[key])
    expected = bytearray(b"\x11" * 4096)
    for index in range(4):
        address = 0x108 + 16 * index
        expected[address:address+8] = bytes(0x40+index*8+i for i in range(8))
    assert (output / "spm.bin").read_bytes() == expected, "unexpected mutation after rejected request"
    with (output / "local.router_spm-spm.csv").open() as source:
        rows = list(csv.DictReader(source))
    for request in range(1, 19):
        own = [row for row in rows if int(row["request_id"]) == request]
        events = [row["event"] for row in own]
        assert "local_response" in events
        assert not any(event in events for event in ("request_send", "response_send", "request_recv", "response_recv"))
        if request in (5, 10, 11, 12, 13, 14):
            assert not any(event in events for event in ("read_request", "write_request"))
        else:
            assert "local_bank_request" in events and "local_bank_complete" in events
            fragments = [row for row in own if row["event"] in ("read_response", "write_response")]
            assert sum(int(row["bytes"]) for row in fragments) == 8
            complete = next(row for row in own if row["event"] == "local_bank_complete")
            assert int(complete["cycle"]) >= max(int(row["cycle"]) for row in fragments)
    with (output / "network-statistics.csv").open() as source:
        stats = [{key.strip(): value.strip() for key, value in row.items()} for row in csv.DictReader(source)]
    traffic = [row for row in stats if row["StatisticName"] in
               ("packets_recv", "recv_flit_cnt", "sent_flit_cnt", "sent_packet_cnt")]
    assert traffic and all(int(row["Sum.u64"]) == 0 for row in traffic)
    assert hashes == {path: sha(path) for path in hashes}, "test inputs changed during run"
    summary = dict(passed=True, endpoint=endpoint, command=command, sha256=hashes,
                   exact_backing_bytes=4096, zero_network_traffic_statistics=len(traffic))
    (output / "validation.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"PASS {output / 'validation.json'}")
if __name__ == "__main__":
    main()
