"""Validate the real 2x2 graph, endpoint results and per-port Mordred traffic."""
from collections import Counter
import csv
from decimal import Decimal
import json
import math
from pathlib import Path
import re
import sys


SOURCE = Path(__file__).resolve().parents[2]
if str(SOURCE) not in sys.path:
    sys.path.insert(0, str(SOURCE))
from components.mordred.configuration import MeshParameters


_ACTIVE_PORTS = {0: (0, 1, 4), 1: (0, 3, 4), 2: (1, 2, 4), 3: (2, 3, 4)}
_PORT_STATS = ("recv_flit_cnt", "sent_flit_cnt", "sent_packet_cnt")


def _time_seconds(value):
    match = re.fullmatch(r"\s*([\d.eE+-]+)\s*(s|ms|us|ns|ps|fs)\s*", value)
    assert match, ("Invalid link latency", value)
    scale = {"s": 0, "ms": -3, "us": -6, "ns": -9, "ps": -12, "fs": -15}
    number = Decimal(match[1]) * Decimal(10) ** scale[match[2]]
    assert number.is_finite() and number > 0, value
    return number


def _parameters(node, expected):
    for key, value in expected.items():
        assert key in node["params"], (node.get("name", node.get("slot_name")), "Missing parameter", key)
        actual = node["params"][key]
        if type(value) is int:
            assert int(actual) == value, (key, actual, value)
        else:
            assert actual == value, (key, actual, value)


def _graph(trial, p, case):
    topology = json.loads((trial / "topology.json").read_text())
    nodes = topology["components"]
    assert len(nodes) == 8, ("Expected four routers and four endpoints", len(nodes))
    routers = [n for n in nodes if n["type"] == "mordred.mordred_router"]
    endpoints = [n for n in nodes if n["type"] == "mordredtests.meshEndpoint"]
    assert len(routers) == len(endpoints) == 4
    routers = {int(node["params"]["id"]): node for node in routers}
    endpoints = {int(node["params"]["id"]): node for node in endpoints}
    assert set(routers) == set(endpoints) == set(range(4))
    assert len({node["name"] for node in nodes}) == 8
    for identity, node in routers.items():
        _parameters(node, dict(id=identity, num_ports=5, num_local_ports=1, num_vns=1,
            num_vcs=p.num_vcs, clock=p.clock, flit_size=f"{p.flit_size_bits}b",
            input_buf_size=f"{p.router_input_buffer_flits * p.flit_size_bits}b",
            output_buf_size=f"{p.router_output_buffer_flits * p.flit_size_bits}b"))
        subcomponents = node.get("subcomponents", [])
        assert len(subcomponents) == 1
        sub, = subcomponents
        assert (sub["slot_name"], int(sub["slot_number"]), sub["type"]) == ("topology", 0, "mordred.MeshTopology")
        assert not sub.get("subcomponents")
        _parameters(sub, dict(xDim=2, yDim=2))
    for identity, node in endpoints.items():
        _parameters(node, dict(id=identity, num_peers=4, num_vns=1, clock=p.clock,
            flit_size_bits=p.flit_size_bits, **case.get("endpoint", {})))
        subcomponents = node.get("subcomponents", [])
        assert len(subcomponents) == 1
        sub, = subcomponents
        assert (sub["slot_name"], int(sub["slot_number"]), sub["type"]) == ("networkIF", 0, "mordred.mordredNIC")
        assert not sub.get("subcomponents")
        _parameters(sub, dict(clock=p.clock, input_buf_size=f"{8 * p.nic_input_buffer_bytes}b",
            output_buf_size=f"{8 * p.nic_output_buffer_bytes}b"))

    def router(identity, port):
        return routers[identity]["name"], f"port{port}"

    # Explicit row-major 2x2 cardinal edges, independent of helper loop ordering.
    expected = {
        frozenset((router(0, 0), router(2, 2))),
        frozenset((router(0, 1), router(1, 3))),
        frozenset((router(1, 0), router(3, 2))),
        frozenset((router(2, 1), router(3, 3))),
    }
    expected.update(frozenset((router(i, 4), (endpoints[i]["name"] + ":networkIF", "port"))) for i in range(4))
    actual, ports = set(), set()
    links = topology["links"]
    assert len(links) == 8 and len({link["name"] for link in links}) == 8
    for link in links:
        ends = []
        for side in ("left", "right"):
            end = link[side]
            component = re.sub(r":networkIF\[0\]$", ":networkIF", end["component"])
            key = component, end["port"]
            assert key not in ports, ("Port has multiple links", key)
            ports.add(key)
            assert _time_seconds(end["latency"]) == _time_seconds(p.link_latency), link
            ends.append(key)
        pair = frozenset(ends)
        assert len(pair) == 2 and pair not in actual
        actual.add(pair)
    assert actual == expected, ("Mesh links/ports differ", actual - expected, expected - actual)
    for identity in range(4):
        for port in range(4):
            assert (router(identity, port) in ports) == (port in _ACTIVE_PORTS[identity])
    return routers, endpoints


def _stat_rows(path):
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream, skipinitialspace=True):
            yield {key.strip(): value.strip() for key, value in row.items()}


def validate_trial(trial, case, records):
    """Check one completed 2x2 all-to-all trial; never modify its artifacts."""
    trial = Path(trial)
    p = MeshParameters(**case.get("mesh", {}))
    assert (p.x_dim, p.y_dim, p.local_ports, p.num_vns) == (2, 2, 1, 1)
    endpoint = case.get("endpoint", {})
    messages, message_bytes = endpoint.get("num_messages", 16), endpoint.get("message_size", 64)
    assert type(messages) is int and messages > 0 and type(message_bytes) is int and message_bytes > 0
    flits = (message_bytes * 8 + p.flit_size_bits - 1) // p.flit_size_bits
    assert flits >= 2
    routers, endpoints = _graph(trial, p, case)
    assert len(records) == 4 and {record["id"] for record in records} == set(range(4))
    per_endpoint = 3 * messages
    blocked_cycles = 0
    for record in records:
        identity = record["id"]
        assert record["passed"] is True and not record["error"], record
        for key, expected in dict(num_peers=4, num_messages_per_peer=messages, message_bytes=message_bytes,
            flit_size_bits=p.flit_size_bits, flits_per_message=flits, num_vns=1,
            expected_messages=per_endpoint, sent=per_endpoint, received=per_endpoint,
            arrival_notifications=per_endpoint, verified_bytes=per_endpoint * message_bytes,
            expected_flits=per_endpoint * flits, recv_delay_cycles=endpoint.get("recv_delay_cycles", 0)).items():
            assert record[key] == expected, (identity, key, record[key], expected)
        peer_counts = [0 if peer == identity else messages for peer in range(4)]
        assert record["sent_per_peer"] == record["received_per_peer"] == peer_counts
        assert record["sent_per_vn"] == record["received_per_vn"] == [per_endpoint]
        assert record["send_attempts"] == per_endpoint + record["send_blocked_cycles"]
        assert record["send_blocked_cycles"] == record["credit_stall_cycles"] + record["send_rejections"] >= 0
        blocked_cycles += record["send_blocked_cycles"]
        assert 0 < record["first_send_tick"] <= record["last_send_tick"]
        assert record["first_send_tick"] < record["first_arrival_tick"] <= record["last_arrival_tick"] <= record["last_receive_tick"]
        for key in ("core_tick_ns", "delivery_latency_ns_min", "delivery_latency_ns_max",
                    "delivery_latency_ns_mean", "receive_latency_ns_mean"):
            assert math.isfinite(record[key]) and record[key] > 0, (identity, key, record[key])
        assert record["delivery_latency_ns_min"] <= record["delivery_latency_ns_mean"] <= record["delivery_latency_ns_max"]
        assert record["receive_latency_ns_mean"] >= record["delivery_latency_ns_mean"]

    router_names = {node["name"]: identity for identity, node in routers.items()}
    nic_names = {node["name"] + ":networkIF": identity for identity, node in endpoints.items()}
    port_stats, nic_stats, xbar_stats = {}, {}, {}
    for row in _stat_rows(trial / "statistics.csv"):
        name, statistic, subid = (row[key] for key in ("ComponentName", "StatisticName", "StatisticSubId"))
        assert row["StatisticType"] == "Accumulator" and int(row["Rank"]) == 0
        if statistic in _PORT_STATS:
            assert name in router_names, ("Port traffic statistic on unknown router", row)
            fields = tuple(int(value) for value in subid.split("_"))
            assert len(fields) == 4
            identity, port, vn, vc = fields
            assert identity == router_names[name] and port in _ACTIVE_PORTS[identity]
            assert vn == 0 and 0 <= vc < p.num_vcs
            key = (*fields, statistic)
            assert key not in port_stats, ("Duplicate traffic statistic", key)
            value = {field: int(row[field]) for field in ("Sum.u64", "SumSQ.u64", "Count.u64", "Min.u64", "Max.u64")}
            assert all(number >= 0 for number in value.values())
            unit = 1 if statistic == "sent_packet_cnt" else flits
            assert value["Sum.u64"] == value["Count.u64"] * unit
            assert value["SumSQ.u64"] == value["Count.u64"] * unit * unit
            if value["Count.u64"]:
                assert value["Min.u64"] == value["Max.u64"] == unit
            port_stats[key] = value
        elif statistic in ("packets_recv", "average_packet_size"):
            name = re.sub(r":networkIF\[0\]$", ":networkIF", name)
            assert name in nic_names and not subid, row
            key = nic_names[name], statistic
            assert key not in nic_stats and int(row["Count.u64"]) == 1
            nic_stats[key] = row
        elif statistic in ("xbar_idle", "xbar_blocked", "flit_unavailable"):
            assert name in router_names and re.fullmatch(r"port[0-4]", subid), row
            identity, port = router_names[name], int(subid[4:])
            key = identity, port, statistic
            assert key not in xbar_stats
            value = int(row["Sum.u64"])
            assert value == int(row["Count.u64"]) >= 0
            if port not in _ACTIVE_PORTS[identity]:
                assert value == 0, ("Activity on an unconnected mesh boundary", row)
            xbar_stats[key] = value
        # output_stalls is registered upstream but never updated. It is not
        # used as evidence of traffic, buffering or backpressure.

    expected_keys = {(identity, port, 0, vc, statistic)
        for identity in range(4) for port in _ACTIVE_PORTS[identity]
        for vc in range(p.num_vcs) for statistic in _PORT_STATS}
    assert set(port_stats) == expected_keys, ("Missing/extra per-VC traffic statistics", expected_keys - set(port_stats))
    port_totals = Counter()
    for identity in range(4):
        for port in _ACTIVE_PORTS[identity]:
            packets = (3 if port == 4 else 2) * messages
            for statistic in _PORT_STATS:
                values = [port_stats[identity, port, 0, vc, statistic] for vc in range(p.num_vcs)]
                expected = packets if statistic == "sent_packet_cnt" else packets * flits
                actual = sum(value["Sum.u64"] for value in values)
                assert actual == expected, (identity, port, statistic, actual, expected)
                assert sum(value["Count.u64"] for value in values) == packets
                port_totals[statistic] += actual
    assert set(nic_stats) == {(i, s) for i in range(4) for s in ("packets_recv", "average_packet_size")}
    for identity in range(4):
        assert int(nic_stats[identity, "packets_recv"]["Sum.u64"]) == per_endpoint
        average = float(nic_stats[identity, "average_packet_size"]["Sum.f64"])
        assert math.isfinite(average) and average == flits
    expected_xbar = {(identity, port, statistic) for identity in range(4) for port in range(5)
        for statistic in ("xbar_idle", "xbar_blocked", "flit_unavailable")}
    assert set(xbar_stats) == expected_xbar
    xbar_blocked = sum(value for (_, _, statistic), value in xbar_stats.items() if statistic == "xbar_blocked")
    tight = case.get("name") == "tight-credits"
    if tight:
        assert blocked_cycles > 0 and xbar_blocked > 0, ("Tight credits failed to exercise backpressure", blocked_cycles, xbar_blocked)

    return dict(passed=True, routers_checked=4, topologies_checked=4, nics_checked=4,
        router_links_checked=4, endpoint_links_checked=4, unconnected_boundary_ports_checked=8,
        messages_per_peer=messages, message_bytes=message_bytes, flits_per_message=flits,
        endpoint_packets_checked=12 * messages, payload_bytes_checked=12 * messages * message_bytes,
        directional_port_packets_each=2 * messages, directional_port_flits_each=2 * messages * flits,
        local_port_packets_each=3 * messages, local_port_flits_each=3 * messages * flits,
        port_vc_statistics_checked=len(port_stats), router_flits_sent=port_totals["sent_flit_cnt"],
        router_flits_received=port_totals["recv_flit_cnt"], router_packets_sent=port_totals["sent_packet_cnt"],
        nic_packets_received=12 * messages, send_blocked_cycles=blocked_cycles,
        xbar_blocked_cycles=xbar_blocked, tight_credit_backpressure_checked=tight,
        exact_graph_and_per_port_traffic=True)
