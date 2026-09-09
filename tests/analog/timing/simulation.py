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
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 8,
        "analog_command_batching": os.environ.get(
            "MITTENS_ANALOG_COMMAND_BATCHING", "false"
        ),
        "profile_mode": "trace",
        "profile_output_directory": required(
            "MITTENS_ANALOG_TIMING_PROFILE"
        ),
        "verbose": 1,
    }
)
