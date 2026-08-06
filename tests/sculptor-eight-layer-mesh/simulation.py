import os
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from support.mesh import build_mesh


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


images = [
    required_path(f"MITTENS_SCULPTOR_EIGHT_LAYER_CORE{core_id}_ELF")
    for core_id in range(4)
]

build_mesh(
    width=2,
    height=2,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required_path("MITTENS_SCULPTOR_EIGHT_LAYER_STATS"),
    verbosity=int(os.environ.get("MITTENS_SCULPTOR_VERBOSITY", "0")),
    tile_params={
        "analog_array_count": 2,
        "analog_array_rows": 8,
        "analog_array_columns": 8,
        "analog_backend": "native",
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 100,
    },
)
