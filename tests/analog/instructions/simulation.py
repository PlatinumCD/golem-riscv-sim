import os

import sst

def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": required_path("MITTENS_TEST_QEMU"),
        "elf": required_path("MITTENS_TEST_ELF"),
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "analog_array_count": 2,
        "analog_array_rows": 4,
        "analog_array_columns": 4,
        "analog_backend": os.environ.get(
            "MITTENS_ANALOG_BACKEND", "native"
        ),
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 100,
        "analog_command_batching": os.environ.get(
            "MITTENS_ANALOG_COMMAND_BATCHING", "false"
        ),
        "verbose": 2,
    }
)

# Program loading and data DMA use the same shared controller.
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support"))
from spm import attach_memory
attach_memory({0: tile})
