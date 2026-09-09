import os
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import build_mesh


build_mesh(
    width=3,
    height=1,
    qemu_path=os.environ["MITTENS_TEST_QEMU"],
    images=[
        os.environ["MITTENS_GLOBAL_RAM_EXACT_TILE0"],
        os.environ["MITTENS_GLOBAL_RAM_EXACT_TILE1"],
        os.environ["MITTENS_GLOBAL_RAM_EXACT_TILE2"],
    ],
    statistics_path=os.environ["MITTENS_GLOBAL_RAM_EXACT_STATS"],
    verbosity=1,
    tile_params={
        "qemu_control_memory": "16M",
        "scratchpad_enabled": True,
        "scratchpad_bytes": 2 * 1024 * 1024,
        "scratchpad_access_batching": True,
        "scratchpad_access_run_compaction": True,
        "global_dma_submit_batching": os.environ.get(
            "MITTENS_TEST_GLOBAL_DMA_SUBMIT_BATCHING", "0"
        ).lower() in ("1", "true", "yes", "on"),
        "profile_mode": "summary",
        "profile_output_directory": os.environ[
            "MITTENS_GLOBAL_RAM_EXACT_PROFILE"
        ],
        "progress_snapshot_interval_ms": 0,
        "progress_watchdog_ms": 10000,
    },
    memory_backend="streaming",
    global_memory={
        "dependency_mode": "exact_dependencies",
        "channels": 1,
        "queue_depth": 24,
        "per_tile_queue_depth": 8,
        "setup_cycles": 8,
        "bytes_per_cycle": 32,
        "burst_bytes": 64,
        "fixed_latency_cycles": 2,
    },
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
)
