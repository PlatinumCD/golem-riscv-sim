import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TEST_DIR.parents[3]
sys.path.insert(0, str(PROJECT_ROOT / "tests" / "support"))

from mesh import build_mesh


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


sst.setProgramOption("timebase", "1ps")

build_mesh(
    width=2,
    height=1,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=[
        required_path("MITTENS_TILE0_ELF"),
        required_path("MITTENS_TILE1_ELF"),
    ],
    statistics_path=required_path("MITTENS_PROFILE_TEST_STATS"),
    verbosity=0,
    tile_params={
        "profile_mode": "trace",
        "profile_output_directory": required_path(
            "MITTENS_PROFILE_TEST_RAW_DIRECTORY"
        ),
    },
)
