#!/usr/bin/env python3
"""Common record construction, validation, and CSV/JSON serialization."""

import json
import math
import re


SCHEMA_VERSION = 2
HISTOGRAM_LENGTH = 5
EXPERIMENT_PATTERN = re.compile(r"^H(?:[1-9]|1[0-5])$")

CSV_COLUMNS = (
    "schema_version", "experiment", "case", "payload_bytes", "num_flows",
    "num_tx_lanes", "num_rx_lanes", "source_tiles", "destination_tiles",
    "mean_hops", "max_hops", "unique_first_hop_directions",
    "bytes_per_first_hop_direction", "makespan_cycles",
    "payload_rate_B_per_cycle", "tx_ready_cycles",
    "tx_serialization_stall_cycles", "tx_active_lane_histogram",
    "tx_bytes_delayed", "tx_fifo_empty_cycles", "tx_fifo_full_cycles",
    "rx_serialization_stall_cycles", "rx_active_lane_histogram",
    "spm_read_bytes", "spm_write_bytes", "spm_read_service_cycles",
    "spm_write_service_cycles", "spm_read_bank_conflicts",
    "spm_write_bank_conflicts", "spm_max_simultaneous_reads",
    "spm_max_simultaneous_writes", "noc_per_link",
    "noc_arbitration_stalls", "noc_backpressure_cycles", "parameters",
    "derived",
)


def empty_record(experiment, case):
    return {
        "schema_version": SCHEMA_VERSION,
        "experiment": experiment,
        "case": case,
        "payload_bytes": 0,
        "num_flows": 0,
        "num_tx_lanes": 1,
        "num_rx_lanes": 1,
        "source_tiles": [],
        "destination_tiles": [],
        "mean_hops": 0.0,
        "max_hops": 0,
        "unique_first_hop_directions": 0,
        "bytes_per_first_hop_direction": {},
        "makespan_cycles": None,
        "payload_rate_B_per_cycle": None,
        "tx": {
            "ready_cycles": None,
            "serialization_stall_cycles": None,
            "active_lane_histogram": None,
            "bytes_delayed": None,
            "fifo_empty_cycles": None,
            "fifo_full_cycles": None,
        },
        "rx": {
            "serialization_stall_cycles": None,
            "active_lane_histogram": None,
        },
        "spm": {
            "read_bytes": None,
            "write_bytes": None,
            "read_service_cycles": None,
            "write_service_cycles": None,
            "read_bank_conflicts": None,
            "write_bank_conflicts": None,
            "max_simultaneous_reads": None,
            "max_simultaneous_writes": None,
        },
        "noc": {
            "per_link": None,
            "arbitration_stalls": None,
            "backpressure_cycles": None,
        },
        "parameters": {},
        "derived": {},
    }


def _nonnegative_number(value, path, integer=False):
    expected = int if integer else (int, float)
    if isinstance(value, bool) or not isinstance(value, expected):
        raise ValueError(f"{path} has the wrong type")
    if value < 0 or (not integer and not math.isfinite(value)):
        raise ValueError(f"{path} must be finite and nonnegative")


def _histogram(value, path):
    if value is None:
        return
    if not isinstance(value, list) or len(value) != HISTOGRAM_LENGTH:
        raise ValueError(f"{path} must contain five buckets")
    for index, item in enumerate(value):
        _nonnegative_number(item, f"{path}[{index}]", integer=True)


def validate_record(record):
    expected = set(empty_record("H1", "case"))
    if set(record) != expected:
        missing = sorted(expected - set(record))
        extra = sorted(set(record) - expected)
        raise ValueError(f"record keys differ: missing={missing}, extra={extra}")
    if record["schema_version"] != SCHEMA_VERSION:
        raise ValueError("unsupported schema_version")
    if not EXPERIMENT_PATTERN.fullmatch(record["experiment"]):
        raise ValueError("experiment must be H1 through H15")
    if not isinstance(record["case"], str) or not record["case"]:
        raise ValueError("case must be a non-empty string")
    for field in ("payload_bytes", "num_flows", "max_hops",
                  "unique_first_hop_directions", "makespan_cycles"):
        _nonnegative_number(record[field], field, integer=True)
    if record["unique_first_hop_directions"] > 4:
        raise ValueError("unique_first_hop_directions exceeds four")
    for field in ("num_tx_lanes", "num_rx_lanes"):
        if record[field] not in (1, 2, 4):
            raise ValueError(f"{field} must be 1, 2, or 4")
    for field in ("source_tiles", "destination_tiles"):
        if not isinstance(record[field], list):
            raise ValueError(f"{field} must be a list")
        for tile in record[field]:
            _nonnegative_number(tile, field, integer=True)
    _nonnegative_number(record["mean_hops"], "mean_hops")
    _nonnegative_number(
        record["payload_rate_B_per_cycle"], "payload_rate_B_per_cycle")
    if not isinstance(record["bytes_per_first_hop_direction"], dict):
        raise ValueError("bytes_per_first_hop_direction must be an object")
    for direction, byte_count in record["bytes_per_first_hop_direction"].items():
        if direction not in ("north", "east", "south", "west", "local"):
            raise ValueError(f"invalid first-hop direction: {direction}")
        _nonnegative_number(byte_count, f"direction {direction}", integer=True)

    required_nested = {
        "tx": ("ready_cycles", "serialization_stall_cycles",
               "active_lane_histogram", "bytes_delayed", "fifo_empty_cycles",
               "fifo_full_cycles"),
        "rx": ("serialization_stall_cycles", "active_lane_histogram"),
        "spm": ("read_bytes", "write_bytes", "read_service_cycles",
                "write_service_cycles", "read_bank_conflicts",
                "write_bank_conflicts", "max_simultaneous_reads",
                "max_simultaneous_writes"),
        "noc": ("per_link", "arbitration_stalls", "backpressure_cycles"),
    }
    for section, fields in required_nested.items():
        value = record[section]
        if not isinstance(value, dict) or any(field not in value for field in fields):
            raise ValueError(f"{section} is missing required fields")
    _histogram(record["tx"]["active_lane_histogram"],
               "tx.active_lane_histogram")
    _histogram(record["rx"]["active_lane_histogram"],
               "rx.active_lane_histogram")
    for section in ("tx", "rx", "spm"):
        for key, value in record[section].items():
            if key.endswith("histogram") or value is None:
                continue
            _nonnegative_number(value, f"{section}.{key}", integer=True)
    if record["noc"]["per_link"] is not None and not isinstance(record["noc"]["per_link"], list):
        raise ValueError("noc.per_link must be a list")
    for field in ("arbitration_stalls", "backpressure_cycles"):
        if record['noc'][field] is not None:
            _nonnegative_number(record["noc"][field], f"noc.{field}", integer=True)
    if not isinstance(record["parameters"], dict) or not isinstance(
            record["derived"], dict):
        raise ValueError("parameters and derived must be objects")
    return record


def flatten_record(record):
    validate_record(record)
    row = {key: record[key] for key in CSV_COLUMNS if key in record}
    row.update({
        "source_tiles": json.dumps(record["source_tiles"], separators=(",", ":")),
        "destination_tiles": json.dumps(record["destination_tiles"], separators=(",", ":")),
        "bytes_per_first_hop_direction": json.dumps(
            record["bytes_per_first_hop_direction"], sort_keys=True,
            separators=(",", ":")),
        "tx_ready_cycles": record["tx"]["ready_cycles"],
        "tx_serialization_stall_cycles": record["tx"]["serialization_stall_cycles"],
        "tx_active_lane_histogram": json.dumps(
            record["tx"]["active_lane_histogram"], separators=(",", ":")),
        "tx_bytes_delayed": record["tx"]["bytes_delayed"],
        "tx_fifo_empty_cycles": record["tx"]["fifo_empty_cycles"],
        "tx_fifo_full_cycles": record["tx"]["fifo_full_cycles"],
        "rx_serialization_stall_cycles": record["rx"]["serialization_stall_cycles"],
        "rx_active_lane_histogram": json.dumps(
            record["rx"]["active_lane_histogram"], separators=(",", ":")),
        **{f"spm_{key}": value for key, value in record["spm"].items()},
        "noc_per_link": json.dumps(record["noc"]["per_link"], sort_keys=True,
                                   separators=(",", ":")),
        "noc_arbitration_stalls": record["noc"]["arbitration_stalls"],
        "noc_backpressure_cycles": record["noc"]["backpressure_cycles"],
        "parameters": json.dumps(record["parameters"], sort_keys=True,
                                  separators=(",", ":")),
        "derived": json.dumps(record["derived"], sort_keys=True,
                               separators=(",", ":")),
    })
    return {column: row[column] for column in CSV_COLUMNS}
