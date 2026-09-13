import os

import sst

def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value

array_count = int(required("MITTENS_ANALOG_TIMING_ARRAY_COUNT"))
if array_count not in (1, 2):
    raise RuntimeError("array count must be one or two")

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": required("MITTENS_TEST_QEMU"),
        "elf": required("MITTENS_TEST_ELF"),
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "cpu_issue_width": 1,
        "analog_array_count": array_count,
        "analog_array_rows": 9,
        "analog_array_columns": 9,
        "analog_backend": "native",
        # Long enough transfers to exercise contention despite timed fetches.
        "analog_link_clock": "100MHz",
        # Leave a real overlap window after instruction-fetch/SPM delays.
        "analog_compute_latency_cycles": 16,
        "profile_mode": "trace",
        "profile_output_directory": required(
            "MITTENS_ANALOG_TIMING_PROFILE"
        ),
        "verbose": 1,
    }
)

# Program loading and data DMA use the same shared controller.
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support"))
from spm import attach_memory
attach_memory({0: tile})
