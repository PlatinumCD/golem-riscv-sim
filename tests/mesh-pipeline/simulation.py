import os
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from support.mesh import build_mesh


MESH_WIDTH = 3
MESH_HEIGHT = 3
NETWORK_SIZE = MESH_WIDTH * MESH_HEIGHT


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


images = [
    required_path(f"MITTENS_PIPELINE_TILE{node_id}_ELF")
    for node_id in range(NETWORK_SIZE)
]

build_mesh(
    width=MESH_WIDTH,
    height=MESH_HEIGHT,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required_path("MITTENS_PIPELINE_STATS"),
    verbosity=1,
)
