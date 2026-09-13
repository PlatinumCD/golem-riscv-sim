#!/usr/bin/env python3
"""Require quantum-independent guest accounting and exact SST finish time."""
import json
import sys
from pathlib import Path


def summary(directory):
    path = Path(directory) / "tile-0-summary.json"
    data = json.loads(path.read_text())
    if data["schema"] != "mittens.summary":
        raise SystemExit(f"{path}: unexpected measurement schema")
    metrics = {metric["name"]: metric for metric in data["metrics"]}
    values = []
    for name in ("instructions", "vector_instructions", "cpu_cycles", "finish_tick"):
        metric = metrics[name]
        if metric["status"] != "available" or type(metric["value"]) is not int:
            raise SystemExit(f"{path}: {name} is not an available integer counter")
        values.append(metric["value"])
    return tuple(values)


values = [summary(path) for path in sys.argv[1:]]
if len(values) < 2 or any(value != values[0] for value in values):
    raise SystemExit(f"quantum changed guest accounting or finish time: {values}")
print(f"quantum-invariant instructions/vector/cpu/finish-tick: {values[0]}")
