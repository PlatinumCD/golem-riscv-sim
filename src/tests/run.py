"""Validate independent banked-SPM and register-payload arrays with real SST."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from build import build, load_build_info
from configuration import resolve


def cases():
    # This driver checks blocking completion and reusable output semantics.
    # The array-pipeline suite covers the default pipelined/FIFO contract.
    shape = dict(array_rows=7, array_cols=13, arrays_per_tile=2, array_pipeline_enabled=False)
    result = []
    def add(name, parameters=None, **options):
        result.append(dict(name=name, parameters=shape | (parameters or {}), **options))
    add("arrays")
    add("arrays-singleton", dict(array_rows=1, array_cols=1, arrays_per_tile=1))
    add("arrays-eight", dict(arrays_per_tile=8))
    # Keep payload sizes fixed across VLEN, so the comparison measures link
    # bandwidth rather than a changing number of guest instructions/chunks.
    for vlen in (128, 256, 512, 1024):
        add(f"arrays-vlen{vlen}", dict(array_rows=16, array_cols=16, arrays_per_tile=1,
            riscv_vector_length_bits=vlen, array_inflight_bytes=128), chunk_elements=32)
    add("arrays-inflight-1", dict(array_inflight_bytes=1))
    add("arrays-inflight-5", dict(array_inflight_bytes=5))
    add("arrays-chunk-1", chunk_elements=1)
    add("arrays-large-chunks", dict(array_rows=32, array_cols=32, array_inflight_bytes=512), chunk_elements=256)
    add("arrays-spm-small", dict(spm_capacity_bytes=4096, spm_banks=1, spm_bank_width=1,
                                spm_channels=1, spm_channel_width=1))
    add("arrays-larger-than-spm", dict(array_rows=64, array_cols=64, arrays_per_tile=1,
                                     spm_capacity_bytes=4096, array_inflight_bytes=8192), chunk_elements=256)
    costs = dict(arrays_per_tile=1, cost_per_array_program_cycles=0, cost_per_mvm_cycles=0)
    add("cost-baseline", costs)
    add("cost-program", costs | dict(cost_per_array_program_cycles=19))
    add("cost-exec", costs | dict(cost_per_mvm_cycles=19))
    add("protocol", dict(arrays_per_tile=1), scenario="protocol")
    for duplex in ("shared", "independent"):
        add(f"duplex-{duplex}", dict(array_rows=32, array_cols=32, arrays_per_tile=2,
            riscv_vector_length_bits=128, array_link_duplex=duplex), scenario="duplex")
    memory = dict(spm_channels=8)
    # Keep the actual default two-channel geometry for a 256-bit bulk request.
    add("memory-vector", scenario="memory-vector")
    add("memory", memory, scenario="memory")
    add("same-bank", memory, scenario="same-bank")
    add("same-bank-two-ports", memory | dict(spm_read_ports_per_bank=2, spm_write_ports_per_bank=2), scenario="same-bank")
    add("memory-one-channel", dict(spm_channels=1), scenario="memory")
    # Explicit wide banks preserve these legacy splitting controls even though
    # default bank-sized probes now carry only four bytes.
    add("memory-narrow-channel", memory | dict(spm_bank_width=32, spm_channel_width=8), scenario="memory")
    add("memory-request-4", memory | dict(spm_bank_width=32, spm_request_bytes=4), scenario="memory")
    add("memory-wide", memory | dict(spm_request_bytes=128, spm_bank_width=128, spm_channel_width=128), scenario="memory")
    add("memory-queue-1", memory, scenario="memory", experimental=dict(queue_entries=1))
    # Bypass Python validation to verify the component enforces its own rule.
    add("invalid-array-link", array_parameter_overrides=dict(array_link_width=1),
        expected_failure="array_link_width must equal riscv_vector_length_bits / 8")
    add("invalid-array-vlen", array_parameter_overrides=dict(riscv_vector_length_bits=192),
        expected_failure="riscv_vector_length_bits must be 128, 256, 512 or 1024")
    return result


def validate_spm(trial, p, stats, vector_requests=False):
    traces = list(trial.glob("scratchpad*.csv"))
    assert len(traces) == 1, traces
    accepted, completed, served = {}, {}, Counter()
    ports, channels, channel_bytes = defaultdict(set), {}, Counter()
    vector_beats = defaultdict(list)
    for row in csv.DictReader(traces[0].open()):
        request, cycle, size = row["id"], int(row["cycle"]), int(row["bytes"])
        if row["event"] == "accepted":
            assert request not in accepted
            assert 0 < size <= p["spm_request_bytes"]
            assert int(row["address"]) % p["spm_request_bytes"] + size <= p["spm_request_bytes"]
            assert row["requestor"].endswith(":memory"), row
            accepted[request] = row
        elif row["event"] == "completed":
            assert request in accepted and request not in completed
            assert cycle > int(accepted[request]["cycle"])
            assert served[request] == int(accepted[request]["bytes"])
            completed[request] = cycle
        else:
            assert row["event"] == "service" and request in accepted and request not in completed
            bank, port, channel = (int(row[key]) for key in ("bank", "port", "channel"))
            address, write = int(row["address"]), int(row["write"])
            assert bank == address // p["spm_bank_width"] % p["spm_banks"]
            assert size <= p["spm_bank_width"] - address % p["spm_bank_width"]
            key = cycle, bank, write
            assert port not in ports[key]
            ports[key].add(port)
            assert port < p["spm_write_ports_per_bank" if write else "spm_read_ports_per_bank"]
            assert row["pool"] == "shared" and channel < p["spm_channels"]
            key = cycle, channel
            assert channels.setdefault(key, request) == request
            channel_bytes[key] += size
            assert channel_bytes[key] <= p["spm_channel_width"]
            assert address == int(accepted[request]["address"]) + served[request]
            served[request] += size
            if vector_requests:
                vector_beats[request].append(row)
    assert len(accepted) == len(completed) == stats["accepted"] == stats["completed"]
    for write, label in (("0", "read_bytes"), ("1", "write_bytes")):
        assert sum(int(row["bytes"]) for row in accepted.values() if row["write"] == write) == stats[label]
    checks = dict(max_request_bytes=max((int(row["bytes"]) for row in accepted.values()), default=0))
    if vector_requests:
        for key, value in dict(spm_banks=8, spm_bank_width=4, spm_channels=2,
                               spm_channel_width=32, spm_request_bytes=32,
                               spm_read_ports_per_bank=1, spm_write_ports_per_bank=1).items():
            assert p[key] == value, (key, p[key])
        assert len(accepted) == 4
        for write, label in (("0", "read"), ("1", "write")):
            requests = [row for row in accepted.values() if row["write"] == write]
            assert len(requests) == 2
            assert {int(row["address"]) for row in requests} == {4096, 4128}
            cycles = []
            for request in requests:
                assert int(request["bytes"]) == 32
                beats = vector_beats[request["id"]]
                assert len(beats) == 8 and all(int(beat["bytes"]) == 4 for beat in beats)
                assert {int(beat["bank"]) for beat in beats} == set(range(8))
                assert {int(beat["port"]) for beat in beats} == {0}
                assert len({beat["channel"] for beat in beats}) == 1
                service_cycles = {int(beat["cycle"]) for beat in beats}
                assert len(service_cycles) == 1, "a full 32-byte request must be served in one cycle"
                cycles.append(service_cycles.pop())
            # Both requests reached the backend before service. A second
            # channel cannot bypass the single port on every addressed bank.
            assert max(int(row["cycle"]) for row in requests) < min(cycles)
            assert len(set(cycles)) == 2 and max(cycles) - min(cycles) == 1
            assert stats[label + "_bank_conflicts"] > 0
            assert stats["max_" + label + "s_serviced_same_cycle"] == 8
            checks[label + "_service_cycles"] = sorted(cycles)
        checks.update(bulk_request_bytes=32, banks_per_request=8, bytes_per_bank=4,
                      service_cycles_per_request=1, peak_bytes_per_direction_per_cycle=32)
    return checks


def validate_arrays(trial, p, stats):
    active, links, total_links = {}, Counter(), Counter()
    link_budget = Counter()
    link_directions = defaultdict(set)
    for row in csv.DictReader((trial / "arrays.csv").open()):
        key, cycle, op = (row["array"], row["token"]), int(row["cycle"]), int(row["operation"])
        assert int(row["buffered"]) <= p["array_inflight_bytes"]
        event = row["event"]
        if event == "start":
            assert key not in active
            active[key] = (cycle, op, int(row["element_count"]))
        elif event.startswith("link_"):
            assert key in active
            count = int(row["bytes"])
            links[key] += count
            total_links[event] += count
            direction = event if p["array_link_duplex"] == "independent" else "shared"
            link_budget[cycle, direction] += count
            assert link_budget[cycle, direction] <= p["array_link_width"]
            link_directions[cycle].add(event)
            active[key] = (*active[key][:3], cycle)
        elif event == "complete":
            start, operation, count, *last_link = active.pop(key)
            assert operation == op
            if op == 2:
                assert count == 0 and links[key] == 0
                assert cycle - start == p["cost_per_mvm_cycles"]
            elif count == 0:
                assert links[key] == 0 and cycle == start
            else:
                assert links[key] == count * 4 and last_link
                delay = p["cost_per_array_program_cycles"] if op == 0 else 0
                assert cycle == last_link[0] + 1 + delay, (row, last_link, delay)
        else:
            assert event == "error", row
            active.pop(key, None)
    assert not active
    assert total_links["link_read"] == stats["link_read_bytes"]
    assert total_links["link_write"] == stats["link_write_bytes"]
    assert stats["peak_buffered_bytes_per_array"] <= p["array_inflight_bytes"]
    return dict(simultaneous_link_directions=any(len(value) == 2 for value in link_directions.values()),
                peak_link_bytes_per_cycle=max(link_budget.values(), default=0))


def validate(trial, case, log):
    p = resolve(case.get("parameters"))
    topology = json.loads((trial / "topology.json").read_text())
    array_nodes = [node for node in topology["components"] if node["type"] == "tilecomponents.AnalogArrays"]
    assert len(array_nodes) == 1
    array_parameters = array_nodes[0]["params"]
    assert int(array_parameters["riscv_vector_length_bits"]) == p["riscv_vector_length_bits"]
    assert int(array_parameters["array_link_width"]) == p["riscv_vector_length_bits"] // 8 == p["array_link_width"]
    reports = {}
    for label in ("SPM_STATS", "ARRAY_STATS", "TEST_RESULT"):
        matches = [json.loads(line[len(label)+1:]) for line in log.splitlines() if line.startswith(label + " ")]
        assert len(matches) == 1, (label, matches)
        reports[label] = matches[0]
    spm, arrays, result = (reports[key] for key in ("SPM_STATS", "ARRAY_STATS", "TEST_RESULT"))
    assert result["passed"] and result["verified_bytes"] > 0
    scenario = case.get("scenario", "arrays")
    reports["SPM_CHECKS"] = validate_spm(trial, p, spm, vector_requests=scenario == "memory-vector")
    reports["ARRAY_CHECKS"] = validate_arrays(trial, p, arrays)
    if scenario in ("memory", "same-bank", "memory-vector"):
        assert arrays["accepted"] == arrays["completed"] == arrays["mvms"] == 0
        expected_bytes = 64 if scenario == "memory-vector" else 128*p["spm_bank_width"]
        assert spm["read_bytes"] == spm["write_bytes"] == result["verified_bytes"] == expected_bytes
    else:
        # This is a structural regression: arrays have no SPM request path.
        assert spm["accepted"] == spm["completed"] == spm["read_bytes"] == spm["write_bytes"] == 0
        assert arrays["errors"] == result["errors"] and arrays["busy"] == result["busy"]
        assert arrays["mvms"] == result["executions"]
        if scenario == "protocol":
            assert result["busy"] == 1 and result["errors"] == 9
            assert arrays["accepted"] == arrays["completed"] == 4
        if scenario == "duplex" and p["array_link_duplex"] == "independent":
            assert reports["ARRAY_CHECKS"]["simultaneous_link_directions"]
    if case["name"] == "memory-queue-1": assert spm["queue_retries"] > 0
    if case["name"] == "memory-request-4":
        assert reports["SPM_CHECKS"]["max_request_bytes"] == 4
        assert spm["accepted"] == 2 * 128 * 32 // 4
    if case["name"] == "memory-narrow-channel":
        assert reports["SPM_CHECKS"]["max_request_bytes"] == 32
        assert spm["read_service_beats"] == spm["write_service_beats"] == 128 * 4
    return reports


def compare(results):
    records = {result["case"]: result for result in results}
    if {"arrays", "arrays-spm-small"} <= records.keys():
        assert records["arrays"]["TEST_RESULT"]["end_cycle"] == records["arrays-spm-small"]["TEST_RESULT"]["end_cycle"]
    fixed_chunks = [f"arrays-vlen{vlen}" for vlen in (128, 256, 512, 1024)]
    if set(fixed_chunks) <= records.keys():
        controls = [records[name] for name in fixed_chunks]
        for vlen, record in zip((128, 256, 512, 1024), controls):
            assert record["ARRAY_CHECKS"]["peak_link_bytes_per_cycle"] == vlen // 8
            assert record["TEST_RESULT"]["programs"] == controls[0]["TEST_RESULT"]["programs"]
            assert record["ARRAY_STATS"]["link_read_bytes"] == controls[0]["ARRAY_STATS"]["link_read_bytes"]
        cycles = [record["TEST_RESULT"]["end_cycle"] for record in controls]
        assert all(left > right for left, right in zip(cycles, cycles[1:])), cycles
    for changed, count in (("cost-program", "programs"), ("cost-exec", "executions")):
        if {"cost-baseline", changed} <= records.keys():
            baseline = records["cost-baseline"]["TEST_RESULT"]
            assert records[changed]["TEST_RESULT"]["end_cycle"] - baseline["end_cycle"] == 19*baseline[count]
    if {"memory", "same-bank", "same-bank-two-ports"} <= records.keys():
        for name, peak in (("memory", 8), ("same-bank", 1), ("same-bank-two-ports", 2)):
            stats = records[name]["SPM_STATS"]
            assert stats["max_reads_serviced_same_cycle"] == peak
            assert stats["max_writes_serviced_same_cycle"] == peak
        assert records["same-bank"]["SPM_STATS"]["read_bank_conflicts"] > 0
    if {"memory", "memory-one-channel"} <= records.keys():
        duration = lambda name: records[name]["TEST_RESULT"]["memory_end"] - records[name]["TEST_RESULT"]["memory_start"]
        assert duration("memory-one-channel") > duration("memory")
    if {"duplex-shared", "duplex-independent"} <= records.keys():
        assert records["duplex-independent"]["TEST_RESULT"]["measured_cycles"] < records["duplex-shared"]["TEST_RESULT"]["measured_cycles"]
    if "memory-wide" in records: assert records["memory-wide"]["SPM_CHECKS"]["max_request_bytes"] == 128


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", help="Select a named regression")
    parser.add_argument("--config", type=Path, help="Run a register-array fixture with JSON architecture settings")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-info", type=Path, help="Reuse a verified component build")
    args = parser.parse_args()
    selected = cases()
    if args.case:
        unknown = set(args.case) - {case["name"] for case in selected}
        if unknown: parser.error(f"Unknown cases: {sorted(unknown)}")
        selected = [case for case in selected if case["name"] in args.case]
    if args.config:
        if args.case: parser.error("--config and --case are mutually exclusive")
        selected = [dict(name="custom", parameters=resolve(json.loads(args.config.read_text())))]
    for bad in ({"clock": "2GHz"}, {"spm_channels": 0}, {"array_link_duplex": "full"},
                {"spm_channels_shared": False}, {"spm_to_array_link_width": 32},
                {"spm_request_bytes": 3}, {"spm_request_bytes": 48},
                {"array_link_width": 0}, {"array_inflight_bytes": True},
                {"cost_per_array_program_cycles": -1}, {"cost_per_mvm_load_cycles": 0}):
        try: resolve(bad)
        except ValueError: pass
        else: raise AssertionError(f"accepted invalid configuration: {bad}")
    output = (args.output or HERE.parents[1] / "tests/results/source-new-components" / str(time.time_ns())).resolve()
    output.mkdir(parents=True)
    print(output, flush=True)
    tools = load_build_info(args.build_info, with_tests=True) if args.build_info else build(output / "build", with_tests=True)
    results = []
    for case in selected:
        trial = output / case["name"]; trial.mkdir()
        (trial / "case.json").write_text(json.dumps(case, indent=2) + "\n")
        (trial / "parameters.json").write_text(json.dumps(resolve(case.get("parameters")), indent=2) + "\n")
        start = time.perf_counter()
        process = subprocess.run([tools["sst"], "--num-threads=1",
            f"--output-json={trial / 'topology.json'}", str(HERE / "simulation.py")],
            env=os.environ | dict(SST_LIB_PATH=tools["plugin"] + ":" + tools["library"],
                TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE="1"),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        (trial / "simulation.log").write_text(process.stdout)
        if case.get("expected_failure"):
            assert process.returncode != 0 and case["expected_failure"] in process.stdout, process.stdout
            results.append(dict(case=case["name"], expected_failure_verified=True))
            (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"PASS {case['name']}: component rejected invalid parameters", flush=True)
            continue
        if process.returncode:
            print(process.stdout[-6000:]); process.check_returncode()
        report = validate(trial, case, process.stdout)
        results.append(dict(case=case["name"], host_wall_seconds=time.perf_counter()-start, **report))
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: {report['TEST_RESULT']['end_cycle']} cycles", flush=True)
    compare(results)
    (output / "validation.json").write_text(json.dumps(dict(passed=True, cases=len(results), controls="passed"), indent=2) + "\n")
    print(f"PASS: {len(results)} independent SST component regressions and controls", flush=True)


if __name__ == "__main__": main()
