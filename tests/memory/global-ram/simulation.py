import os
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import build_mesh


build_mesh(
    width=2,
    height=1,
    qemu_path=os.environ["MITTENS_TEST_QEMU"],
    images=[os.environ["MITTENS_GLOBAL_RAM_TILE0"],
            os.environ["MITTENS_GLOBAL_RAM_TILE1"]],
    statistics_path=os.environ["MITTENS_GLOBAL_RAM_STATS"],
    verbosity=1,
    tile_params={
        "qemu_control_memory": "16M",
        "scratchpad_enabled": True,
        "scratchpad_bytes": 2 * 1024 * 1024,
    },
    memory_backend="streaming",
    global_memory={
        "channels": int(os.environ["MITTENS_GLOBAL_RAM_CHANNELS"]),
        "queue_depth": 8,
        "per_tile_queue_depth": 4,
        "setup_cycles": 8,
        "bytes_per_cycle": 32,
        "burst_bytes": 64,
        "fixed_latency_cycles": 200,
    },
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
)
