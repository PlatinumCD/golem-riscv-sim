"""Independent byte, protocol, bank-access and resource accounting oracles."""
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import struct
import sys

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE))
from configuration import resolve

WORDS = 64
BASE = 0x90000000
READY, NOTIFY, DONE, RESULT = 0x100000, 0x100100, 0x100200, 0x101000
SRC, INPUT, DENSE, OUTPUT, WEIGHTS, PROBE = 0x110000, 0x120000, 0x130000, 0x140000, 0x150000, 0x160000


def reports(log, label):
    return [json.loads(line[len(label) + 1:]) for line in log.splitlines() if line.startswith(label + " ")]


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def word_address(base, word, p):
    width, banks = p["spm_bank_width"], p["spm_banks"]
    group, within = divmod(word, width // 4)
    stripe, selected = divmod(group, 2)
    return base + stripe * banks * width + p["router_spm_banks"][selected] * width + 4 * within


def payload(owner, count=WORDS):
    return struct.pack(f"<{count}f", *((32 * owner + index) % 128 for index in range(count)))


def initial_image(elf, capacity):
    data = Path(elf).read_bytes()
    assert data[:6] == b"\x7fELF\x02\x01"
    image = bytearray(capacity)
    table, = struct.unpack_from("<Q", data, 32)
    size, count = struct.unpack_from("<HH", data, 54)
    for index in range(count):
        kind, _, offset, _, physical, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, table + index * size)
        if kind != 1:
            continue
        address = physical - BASE
        assert 0 <= address <= address + memsz <= capacity
        image[address:address + filesz] = data[offset:offset + filesz]
    return image


def check_graph(trial, p, cycle_profiling=False):
    graph = json.loads((trial / "topology.json").read_text())
    components = {node["name"]: node for node in graph["components"]}
    expected = {"tilecomponents.RiscvQemu": 4, "tilecomponents.AnalogArrays": 4,
                "tilecomponents.Scratchpad": 4, "tilecomponents.MordredSpmEndpoint": 4,
                "mordred.mordred_router": 4,
                ("tilecomponents.ProfiledSpmConnections" if cycle_profiling else "memHierarchy.Bus"): 4,
                "tilecomponents.SharedBankInitiator": 4}
    assert Counter(node["type"] for node in components.values()) == expected
    edges = [tuple((link[side]["component"].replace("[0]", ""), link[side]["port"])
                   for side in ("left", "right")) for link in graph["links"]]
    assert len(edges) == 32
    edge_set = {frozenset(edge) for edge in edges}
    for identity in range(4):
        prefix = f"tile_mesh.tile{identity}."
        cpu, endpoint, spm, arrays, bus = (prefix + name for name in ("riscv", "router_spm", "scratchpad", "arrays", "spm_connections"))
        assert components[spm]["params"]["external_write_requestor"] == cpu + ":qemu_memory"
        for edge in [((cpu + ":qemu_memory", "lowlink"), (bus, "highlink0")),
                     ((endpoint + ":memory", "lowlink"), (bus, "highlink1")),
                     ((bus, "lowlink0"), (spm, "highlink")),
                     ((cpu, "external_commit"), (spm, "external_commit")),
                     ((cpu, "analog_commands"), (arrays, "commands")),
                     ((f"tile_mesh.client{identity}", "requests"), (endpoint, "requests"))]:
            assert frozenset(edge) in edge_set, edge
        assert len([edge for edge in edges if any(name == arrays for name, _ in edge)]) == 1
        assert {sub["slot_name"]: sub["type"] for sub in components[endpoint]["subcomponents"]} == {
            "memory": "memHierarchy.standardInterface", "networkIF": "mordred.mordredNIC"}
    return dict(cpu_tiles=4, routers=4, physical_banks_per_tile=p["spm_banks"],
                cpu_bank_ids=p["cpu_spm_banks"], router_bank_ids=p["router_spm_banks"],
                shared_backing_and_bank_scheduler=True, arrays_connect_only_to_cpu=True)


def client_checks(trial, source, p):
    records, pending, peak = {}, set(), 0
    signatures = []
    for raw in rows(trial / f"initiator{source}.csv"):
        row = {key: value if key in ("event", "label", "data_hex") else int(value) for key, value in raw.items()}
        signatures.append(row)
        identity = row["id"]
        assert row["source"] == source and row["destination"] == (source + 1) % 4
        if row["event"] == "request":
            assert identity not in records and row["status"] == 0
            records[identity] = dict(request=row)
            pending.add(identity); peak = max(peak, len(pending))
        else:
            assert row["event"] == "response" and identity in pending
            request = records[identity]["request"]
            for key in ("label", "source", "destination", "address", "bytes", "write"):
                assert row[key] == request[key]
            assert row["cycle"] > request["cycle"]
            records[identity]["response"] = row; pending.remove(identity)
    assert not pending and peak == 5
    expected_status = {"forbidden": 3, "mixed": 3, "bounds": 2, "malformed": 1, "zero-length": 1, "window-full": 4}
    counts = Counter()
    for record in records.values():
        request, response = record["request"], record["response"]
        label = request["label"]; counts[label] += 1
        assert response["status"] == expected_status.get(label, 0)
        data = bytes.fromhex(response["data_hex"])
        if response["status"] or request["write"]:
            assert not data
        elif label in ("ready", "done"):
            assert len(data) == 4
            value, = struct.unpack("<I", data)
            assert value in (0, ({"ready": 0x52445900, "done": 0x444f4e00}[label] | request["destination"]))
        elif label == "source":
            matching = [i for i in range(WORDS) if word_address(SRC, i, p) == request["address"]]
            assert len(matching) == 1
            first, = matching
            assert data == payload(request["destination"])[first * 4:first * 4 + request["bytes"]]
        else:
            raise AssertionError(label)
        if label == "input":
            first, = [i for i in range(WORDS) if word_address(INPUT, i, p) == request["address"]]
            assert bytes.fromhex(request["data_hex"]) == payload(source)[first * 4:first * 4 + request["bytes"]]
    assert counts["forbidden"] == counts["mixed"] == counts["bounds"] == 2
    assert all(counts[key] == 1 for key in ("malformed", "zero-length", "window-full", "notify"))
    assert counts["source"] == counts["input"] == WORDS * 4 // p["spm_bank_width"]
    notify, = [record for record in records.values() if record["request"]["label"] == "notify"]
    assert bytes.fromhex(notify["request"]["data_hex"]) == struct.pack("<I", 0x4e4f5400 | (source + 1) % 4)
    assert max(record["response"]["cycle"] for record in records.values()
               if record["request"]["label"] in ("input", *expected_status)) < notify["request"]["cycle"]
    return records, hashlib.sha256(json.dumps(signatures, sort_keys=True).encode()).hexdigest()


def bank_checks(trial, identity, p, cpu, spm, transactions):
    prefix = f"tile_mesh.tile{identity}."
    traces = list(trial.glob(prefix + "scratchpad*.csv")); assert len(traces) == 1
    accepted, completed, served = {}, {}, Counter()
    ports, channel_bytes, channel_owner = set(), Counter(), {}
    activity = defaultdict(lambda: [0, 0]); signatures = []
    cpu_name, router_name = prefix + "riscv:qemu_memory", prefix + "router_spm:memory"
    canonical_ids = {}
    for row in rows(traces[0]):
        request, cycle, size, write = row["id"], int(row["cycle"]), int(row["bytes"]), int(row["write"])
        address = int(row["address"])
        assert row["requestor"] in (cpu_name, router_name)
        signature = dict(row); signature["id"] = canonical_ids.setdefault(request, len(canonical_ids))
        signatures.append(signature)
        router = row["requestor"] == router_name
        allowed = p["router_spm_banks" if router else "cpu_spm_banks"]
        if row["event"] == "accepted":
            assert request not in accepted and 0 < size <= p["spm_request_bytes"]
            assert address % p["spm_request_bytes"] + size <= p["spm_request_bytes"]
            assert all((byte // p["spm_bank_width"]) % p["spm_banks"] in allowed for byte in range(address, address + size))
            accepted[request] = row
            activity[cycle][router] += 1
        elif row["event"] == "completed":
            assert request in accepted and request not in completed
            assert cycle > int(accepted[request]["cycle"]) and served[request] == int(accepted[request]["bytes"])
            completed[request] = cycle; activity[cycle][router] -= 1
        else:
            assert row["event"] == "service" and request in accepted and request not in completed
            bank, port, channel = (int(row[key]) for key in ("bank", "port", "channel"))
            assert bank in allowed and bank == address // p["spm_bank_width"] % p["spm_banks"]
            assert 0 < size <= p["spm_bank_width"] - address % p["spm_bank_width"]
            assert 0 <= port < p["spm_write_ports_per_bank" if write else "spm_read_ports_per_bank"]
            key = cycle, bank, write, port
            assert key not in ports; ports.add(key)
            assert row["pool"] == "shared" and 0 <= channel < p["spm_channels"]
            key = cycle, channel
            assert channel_owner.setdefault(key, request) == request
            channel_bytes[key] += size; assert channel_bytes[key] <= p["spm_channel_width"]
            assert address == int(accepted[request]["address"]) + served[request]
            served[request] += size
    assert len(accepted) == len(completed) == spm["accepted"] == spm["completed"]
    cpu_requests = [row for row in accepted.values() if row["requestor"] == cpu_name]
    router_requests = [row for row in accepted.values() if row["requestor"] == router_name]
    assert len(cpu_requests) == cpu["memory_requests"] == cpu["completed_requests"]
    for direction, key in (("0", "read_bytes"), ("1", "write_bytes")):
        assert sum(int(row["bytes"]) for row in accepted.values() if row["write"] == direction) == spm[key]
    successful = {key: record for key, record in transactions.items() if not record["response"]["status"]}
    fragments = defaultdict(list)
    for row in router_requests:
        address, size, write, cycle = (int(row[key]) for key in ("address", "bytes", "write", "cycle"))
        matches = [key for key, record in successful.items() if record["request"]["write"] == write and
            record["request"]["address"] <= address and address + size <= record["request"]["address"] + record["request"]["bytes"] and
            record["request"]["cycle"] < cycle < completed[row["id"]] < record["response"]["cycle"]]
        assert len(matches) == 1, (identity, row, matches)
        fragments[matches[0]].append(row)
    assert set(fragments) == set(successful)
    for key, record in successful.items():
        pieces = sorted(fragments[key], key=lambda row: int(row["address"]))
        address = record["request"]["address"]
        for piece in pieces:
            assert int(piece["address"]) == address
            address += int(piece["bytes"])
        assert address == record["request"]["address"] + record["request"]["bytes"]
    assert not [row for row in router_requests if PROBE <= int(row["address"]) < PROBE + 4096], "Rejected request reached memory"
    writes = [row for row in router_requests if row["write"] == "1" and INPUT <= int(row["address"]) < INPUT + 4096]
    reads = [row for row in cpu_requests if row["write"] == "0" and INPUT <= int(row["address"]) < INPUT + 4096]
    assert sum(int(row["bytes"]) for row in writes) == sum(int(row["bytes"]) for row in reads) == WORDS * 4
    assert max(completed[row["id"]] for row in writes) < min(int(row["cycle"]) for row in reads)
    source_writes = [row for row in cpu_requests if row["write"] == "1" and SRC <= int(row["address"]) < SRC + 4096]
    source_reads = [row for row in router_requests if row["write"] == "0" and SRC <= int(row["address"]) < SRC + 4096]
    assert sum(int(row["bytes"]) for row in source_reads) == WORDS * 4
    assert max(completed[row["id"]] for row in source_writes) < min(int(row["cycle"]) for row in source_reads)
    pending, previous, overlap = [0, 0], 0, 0
    for cycle, change in sorted(activity.items()):
        if min(pending) > 0: overlap += cycle - previous
        pending = [pending[i] + change[i] for i in range(2)]
        assert min(pending) >= 0
        previous = cycle
    assert pending == [0, 0]
    signature = hashlib.sha256(json.dumps(signatures, sort_keys=True).encode()).hexdigest()
    return dict(cpu_requests=len(cpu_requests), router_requests=len(router_requests),
                router_read_bytes=sum(int(row["bytes"]) for row in router_requests if row["write"] == "0"),
                router_write_bytes=sum(int(row["bytes"]) for row in router_requests if row["write"] == "1"),
                simultaneous_pending_cycles=overlap, rejected_ranges_never_admitted=True,
                cpu_publish_before_remote_read=True, remote_completion_before_cpu_read=True), signature


def endpoint_checks(trial, case, transactions, endpoints, tiles):
    grouped = defaultdict(lambda: defaultdict(list))
    events = []
    for identity in range(4):
        for raw in rows(trial / f"tile_mesh.tile{identity}.router_spm-spm.csv"):
            row = {key: value if key == "event" else int(value) for key, value in raw.items()}
            grouped[row["source"], row["request_id"]][row["event"]].append(row)
            events.append(row)
    flit_bytes = case.get("mesh_parameters", {}).get("flit_size_bits", 128) // 8
    wire = lambda count: max(2, (40 + count + flit_bytes - 1) // flit_bytes) * flit_bytes
    sent, received, counters = Counter(), Counter(), defaultdict(Counter)
    for source, records in transactions.items():
        for identity, record in records.items():
            request, response = record["request"], record["response"]
            target, status = request["destination"], response["status"]
            timeline = grouped.pop((source, identity))
            local, = timeline["local_response"]
            assert local["tile"] == source and local["status"] == status
            assert request["cycle"] < local["cycle"] < response["cycle"]
            if status in (1, 4):
                assert set(timeline) == {"local_response"}
                counters[source]["local_rejected"] += 1
                continue
            chain = ["local_request", "request_send", "request_recv", "response_ready",
                     "response_send", "response_recv", "local_response"]
            singles = []
            for name in chain:
                one, = timeline[name]
                assert one["tile"] == (target if name in ("request_recv", "response_ready", "response_send") else source)
                singles.append(one)
            assert [row["cycle"] for row in singles] == sorted(row["cycle"] for row in singles)
            assert timeline["request_send"][0]["cycle"] < timeline["request_recv"][0]["cycle"]
            assert timeline["response_send"][0]["cycle"] < timeline["response_recv"][0]["cycle"]
            counters[source]["requests_sent"] += 1; counters[target]["requests_received"] += 1
            counters[target]["responses_sent"] += 1; counters[source]["responses_received"] += 1
            counters[source]["local_completed"] += 1
            request_wire = wire(request["bytes"] if request["write"] else 0)
            response_wire = wire(response["bytes"] if not status and not request["write"] else 0)
            sent[source] += request_wire; received[target] += request_wire
            sent[target] += response_wire; received[source] += response_wire
            if status:
                assert status in (2, 3) and set(timeline) == set(chain), "Rejected request issued memory work"
                counters[target]["remote_rejected"] += 1
                continue
            counters[target]["remote_completed"] += 1
            kind = "write" if request["write"] else "read"
            starts, finishes = timeline[kind + "_request"], timeline[kind + "_response"]
            assert set(timeline) == {*chain, kind + "_request", kind + "_response"}
            by_id = {row["memory_request_id"]: row for row in starts}
            assert len(by_id) == len(starts) == len(finishes)
            assert sum(row["bytes"] for row in starts) == request["bytes"]
            for finish in finishes:
                start = by_id.pop(finish["memory_request_id"])
                assert start["address"] == finish["address"] and start["bytes"] == finish["bytes"]
                assert singles[2]["cycle"] <= start["cycle"] < finish["cycle"] <= singles[3]["cycle"]
            assert not by_id
    assert not grouped
    for identity in range(4):
        endpoint = endpoints[f"tile_mesh.tile{identity}.router_spm"]
        assert endpoint["idle"] is True and endpoint["tile_id"] == identity
        for key, count in counters[identity].items():
            assert endpoint[key] == count, (identity, key, endpoint[key], count)
        assert endpoint["local_rejected"] == 3 and endpoint["remote_rejected"] == 6
        assert endpoint["wire_bytes_sent"] == sent[identity] and endpoint["wire_bytes_received"] == received[identity]
        assert endpoint["packets_injected"] == endpoint["requests_sent"] + endpoint["responses_sent"]
        assert endpoint["max_local_requests"] == 4 and 0 < endpoint["max_incoming_requests"] <= 4
        assert 0 < endpoint["max_pending_memory"] <= case.get("router_parameters", {}).get("memory_queue_depth", 8)
        assert endpoint["max_pending_nic_packets"] > 0
        assert endpoint["bytes_read"] == tiles[identity]["spm"]["router_read_bytes"]
        assert endpoint["bytes_written"] == tiles[identity]["spm"]["router_write_bytes"]
    return dict(requests_cross_real_network=True, response_after_all_memory_completions=True,
                forbidden_requests_rejected_at_target_before_memory=True,
                exact_flit_rounded_wire_bytes=sum(sent.values()), finite_request_and_memory_windows=True)


def validate_trial(trial, case, returncode=0):
    trial = Path(trial)
    p = resolve(case.get("parameters"), cpu_parameters=case.get("cpu_parameters"))
    log = (trial / "simulation.log").read_text()
    tiles = json.loads((trial / "tiles.json").read_text())
    assert len({Path(tile["memory_file"]).stat().st_ino for tile in tiles}) == 4
    if case.get("expected_cpu_rejection"):
        assert returncode != 0 and "SPM bank connection denied:" in log and ":qemu_memory" in log
        assert "bank=0" in log
        for identity, tile in enumerate(tiles):
            assert Path(tile["memory_file"]).read_bytes() == initial_image(case["elfs"][identity], p["spm_capacity_bytes"])
        return dict(expected_cpu_bank_rejection=True, unchanged_after_elf_boot_bytes=4 * p["spm_capacity_bytes"])
    assert returncode == 0
    topology = check_graph(trial, p, case.get("cycle_profiling", False))
    cpu = {record["component"]: record for record in reports(log, "RISCV_STATS")}
    spm = {record["component"]: record for record in reports(log, "SPM_STATS")}
    arrays = {record["component"]: record for record in reports(log, "ARRAY_STATS")}
    endpoints = {record["component"]: record for record in reports(log, "MORDRED_SPM_STATS")}
    clients = {record["tile"]: record for record in reports(log, "SHARED_BANK_CLIENT_STATS")}
    assert len(cpu) == len(spm) == len(arrays) == len(endpoints) == len(clients) == 4
    transactions, signatures = {}, {}
    for identity in range(4):
        transactions[identity], signatures[f"client{identity}"] = client_checks(trial, identity, p)
    checks = []
    for identity, tile in enumerate(tiles):
        data = Path(tile["memory_file"]).read_bytes()
        expected = initial_image(case["elfs"][identity], p["spm_capacity_bytes"])
        result = struct.unpack_from("<16I", data, RESULT)
        owner = (identity + 3) % 4
        source, incoming, output = payload(identity), payload(owner), payload(owner, 32)
        checksum = lambda b: sum(struct.unpack(f"<{len(b)//4}I", b)) % 2**32
        exact_result = (0x53424e4b, identity, 1, 5, WORDS * 4, checksum(incoming), 32, checksum(output),
                        WORDS, 2 * p["spm_banks"] * p["spm_bank_width"], 0, 0, 0,
                        p["spm_bank_width"], *p["router_spm_banks"])
        assert result == exact_result, (identity, result, exact_result)
        expected[RESULT:RESULT + 64] = struct.pack("<16I", *exact_result)
        for offset, values in ((SRC, source), (INPUT, incoming)):
            for word in range(WORDS):
                address = word_address(offset, word, p)
                expected[address:address + 4] = values[4 * word:4 * word + 4]
        for offset, values in ((DENSE, incoming), (OUTPUT, output),
            (PROBE, b"\x5a" * (2 * p["spm_banks"] * p["spm_bank_width"])),
            (WEIGHTS, struct.pack("<1024f", *(int(index % 33 == 0) for index in range(1024))))):
            expected[offset:offset + len(values)] = values
        for offset, flag in ((READY, 0x52445900), (NOTIFY, 0x4e4f5400), (DONE, 0x444f4e00)):
            address = offset + p["router_spm_banks"][0] * p["spm_bank_width"]
            expected[address:address + 4] = struct.pack("<I", flag | identity)
        # Stack values legitimately vary with register allocation. Every other
        # byte, including all private banks and unused payload stripes, is exact.
        expected[0x1ef000:0x1ff000] = data[0x1ef000:0x1ff000]
        if data != expected:
            mismatch = next(index for index, pair in enumerate(zip(data, expected)) if pair[0] != pair[1])
            raise AssertionError(("Unexpected SPM byte", identity, hex(mismatch), data[mismatch], expected[mismatch]))
        prefix = f"tile_mesh.tile{identity}."
        c, a = cpu[prefix + "riscv"], arrays[prefix + "arrays"]
        s, = (record for name, record in spm.items() if name.startswith(prefix + "scratchpad:"))
        assert c["vector_instructions"] > 0 and c["memory_requests"] == c["completed_requests"]
        assert a["accepted"] == a["completed"] == c["analog_commands"] and a["mvms"] == 1 and a["errors"] == 0
        banking, signatures[f"banks{identity}"] = bank_checks(trial, identity, p, c, s, transactions[owner])
        checks.append(dict(id=identity, result=list(result), cpu=c, arrays=a, spm=banking))
        client = clients[identity]
        assert client["passed"] and client["requests"] == client["responses"]
        assert client["rejections"] == 8 and client["busy"] == 1 and client["max_pending"] == 5
        assert client["source_bytes_checked"] == client["input_bytes_written"] == WORDS * 4
    assert sum(tile["spm"]["simultaneous_pending_cycles"] for tile in checks) > 0
    protocol = endpoint_checks(trial, case, transactions, endpoints, checks)
    return dict(topology=topology, tiles=checks, endpoints=endpoints, clients=clients, protocol=protocol,
                trace_signatures=signatures, verified_payload_bytes=4 * WORDS * 4,
                exact_nonstack_bytes_checked=4 * (p["spm_capacity_bytes"] - 65536),
                mvms_using_received_data=4, rejected_router_requests=36,
                end_cycle=max(record["end_cycle"] for record in cpu.values()))
