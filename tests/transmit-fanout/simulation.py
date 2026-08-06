import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parent / "support"))

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
        required_path("MITTENS_FANOUT_TILE0_ELF"),
        required_path("MITTENS_FANOUT_TILE1_ELF"),
    ],
    statistics_path=required_path("MITTENS_FANOUT_STATS"),
    verbosity=0,
    tile_params={
        "cpu_clock": "1GHz",
        "cpu_issue_width": 2,
        "sync_instruction_quantum": int(
            required_path("MITTENS_FANOUT_SYNC_QUANTUM")
        ),
        "profile_mode": "trace",
        "profile_output_directory": required_path(
            "MITTENS_FANOUT_PROFILE_RAW"
        ),
    },
    # Preserve 64 KiB of buffering while modeling one physical 32-bit word
    # per timing cell instead of padding each request to a 16 KiB cell.
    network_cell_words=1,
    network_buffer_cells=16384,
    mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
)
