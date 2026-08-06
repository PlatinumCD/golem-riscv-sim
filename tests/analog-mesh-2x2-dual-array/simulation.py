import os
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from support.mesh import build_mesh


MESH_WIDTH = 2
MESH_HEIGHT = 2
NETWORK_SIZE = MESH_WIDTH * MESH_HEIGHT


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


images = [
    required_path(f"MITTENS_DUAL_ARRAY_TILE{tile_id}_ELF")
    for tile_id in range(NETWORK_SIZE)
]

build_mesh(
    width=MESH_WIDTH,
    height=MESH_HEIGHT,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required_path("MITTENS_DUAL_ARRAY_STATS"),
    verbosity=0,
    tile_params={
        "analog_array_count": 2,
        "analog_array_rows": 4,
        "analog_array_columns": 4,
        "analog_backend": os.environ.get(
            "MITTENS_ANALOG_BACKEND", "native"
        ),
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 100,
    },
)
