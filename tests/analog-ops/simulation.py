import os

import sst


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


# Deliberately no networkIF, router, or Merlin component: this test exercises
# one QEMU tile and one tile-local analog array only.
tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": required_path("MITTENS_TEST_QEMU"),
        "elf": required_path("MITTENS_TEST_ELF"),
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "analog_array_count": 1,
        "analog_array_rows": 4,
        "analog_array_columns": 4,
        "analog_backend": os.environ.get(
            "MITTENS_ANALOG_BACKEND", "native"
        ),
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 8,
        "verbose": 1,
    }
)
