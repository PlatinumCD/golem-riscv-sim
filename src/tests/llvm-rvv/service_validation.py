"""Stream and validate the measured CPU vector/SPM transfer and bank service."""
import csv
import json
from profiling import enabled as profiling_enabled


CACHE_DEFAULTS = dict(instruction_cache_enabled=True, instruction_cache_bytes=8192,
    instruction_cache_line_bytes=64, instruction_cache_ways=2, instruction_cache_hit_cycles=1)


def _stats(log, label):
    found = [json.loads(line[len(label) + 1:]) for line in log.splitlines()
             if line.startswith(label + " ")]
    assert len(found) == 1, ("Missing or duplicate statistics", label, len(found))
    return found[0]


def _parameter(actual, expected, key):
    if isinstance(expected, bool):
        assert str(actual).lower() in ("true", "false", "0", "1"), (key, actual)
        actual = str(actual).lower() in ("true", "1")
    elif isinstance(expected, int):
        actual = int(actual)
    assert actual == expected, ("Topology parameter mismatch", key, actual, expected)


def _cache(counters, line_bytes):
    assert counters["icache_fetches"] == counters["icache_hits"] + counters["icache_misses"], counters
    assert counters["icache_misses"] <= counters["icache_fills"] <= 2 * counters["icache_misses"], counters
    assert counters["fetch_bytes"] == counters["icache_fill_bytes"] == counters["icache_fills"] * line_bytes, counters


def _topology(trial, case):
    p = case["parameters"]
    topology = json.loads((trial / "topology.json").read_text())
    nodes = topology["components"]
    assert not any(node["type"] == "tilecomponents.AnalogArrays" for node in nodes), "Memory-only fixture must not instantiate arrays"
    cpu, = [node for node in nodes if node["type"] == "tilecomponents.RiscvQemu"]
    scratch, = [node for node in nodes if node["type"] == "tilecomponents.Scratchpad"]
    bus_type = "tilecomponents.ProfiledSpmConnections" if profiling_enabled() else "memHierarchy.Bus"
    bus, = [node for node in nodes if node["type"] == bus_type]
    assert len(nodes) == 3, ("Unexpected component in CPU/SPM-only topology", nodes)
    assert scratch["params"]["clock"] == bus["params"]["bus_frequency"] == "1GHz"
    assert scratch["params"]["backing"] == "mmap"
    converter, = scratch["subcomponents"]
    backend, = converter["subcomponents"]
    assert converter["type"] == "tilecomponents.ExactConvertor" and backend["type"] == "tilecomponents.BankedBackend"
    for key, value in p.items():
        if key.startswith("spm_") and key not in ("spm_capacity_bytes", "spm_request_bytes"):
            _parameter(backend["params"][key], value, key)
    for key in ("spm_capacity_bytes", "spm_request_bytes", "riscv_vector_length_bits"):
        _parameter(cpu["params"][key], p[key], key)
    capacity = f'{p["spm_capacity_bytes"]}B'
    assert scratch["params"]["size"] == backend["params"]["mem_size"] == capacity
    for key in ("scratch_line_size", "memory_line_size"):
        _parameter(scratch["params"][key], p["spm_request_bytes"], key)
    for node in (converter, backend):
        _parameter(node["params"]["request_width"], p["spm_request_bytes"], "request_width")
    interface, = cpu["subcomponents"]
    assert interface["slot_name"] == "qemu_memory" and interface["type"] == "memHierarchy.standardInterface"
    cache = CACHE_DEFAULTS | {key: value for key, value in case.get("cpu_parameters", {}).items()
                             if key.startswith("instruction_cache_")}
    assert cache["instruction_cache_enabled"], "This fixture requires the instruction cache"
    for key, expected in cache.items():
        _parameter(cpu["params"][key], expected, key)
    for key, expected in case.get("cpu_parameters", {}).items():
        _parameter(cpu["params"][key], expected, key)
    assert not any(end["port"] == "analog_commands" for link in topology["links"]
                   for end in (link["left"], link["right"])), "Unexpected array command link"
    return cpu["name"], cache


def _phases(trial, cpu_name, line_bytes):
    with (trial / f"{cpu_name}-tasks.csv").open() as stream:
        markers = list(csv.DictReader(stream))
    assert len(markers) == 6, ("Expected warmup, empty and transfer marker pairs", len(markers))
    phases = {}
    previous_finish = -1
    for index, task_id in enumerate((2, 0, 1)):
        pair = [{key: value if key == "event" else int(value) for key, value in row.items()}
                for row in markers[index * 2:index * 2 + 2]]
        start, finish = pair
        assert start["event"] == "start" and finish["event"] == "finish", pair
        assert start["task_id"] == finish["task_id"] == task_id, pair
        assert start["execution_id"] == finish["execution_id"] == 0, pair
        assert previous_finish < start["cycle"] < finish["cycle"], pair
        previous_finish = finish["cycle"]
        assert all(row["memory_requests"] == row["completed_requests"] for row in pair), pair
        phase = dict(start_cycle=start["cycle"], end_cycle=finish["cycle"],
            **{("cycles" if key == "cycle" else key): finish[key] - start[key]
               for key in start if key not in ("event", "task_id", "execution_id")})
        assert all(value >= 0 for value in phase.values()), phase
        _cache(phase, line_bytes)
        phases[task_id] = phase
    return phases
