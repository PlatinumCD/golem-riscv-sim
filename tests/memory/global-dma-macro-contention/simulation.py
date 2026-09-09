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
    images=[
        os.environ["MITTENS_GLOBAL_DMA_MACRO_TILE0"],
        os.environ["MITTENS_GLOBAL_DMA_MACRO_TILE1"],
    ],
    statistics_path=os.environ["MITTENS_GLOBAL_DMA_MACRO_STATS"],
    verbosity=1,
    tile_params={
        "qemu_control_memory": "16M",
        "scratchpad_enabled": True,
        "scratchpad_bytes": 2 * 1024 * 1024,
        "global_dma_macro_execution": os.environ.get(
            "MITTENS_TEST_GLOBAL_DMA_MACRO_EXECUTION", "0"
        ).lower()
        in ("1", "true", "yes", "on"),
        "sync_instruction_quantum": 1_000_000,
        "profile_mode": "trace",
        "profile_output_directory": os.environ[
            "MITTENS_GLOBAL_DMA_MACRO_PROFILE"
        ],
        "serial_output_directory": os.environ[
            "MITTENS_GLOBAL_DMA_MACRO_UART"
        ],
        "progress_snapshot_interval_ms": 0,
        "progress_watchdog_ms": 10_000,
    },
    memory_backend="streaming",
    global_memory={
        "dependency_mode": "exact_dependencies",
        "channels": 1,
        "queue_depth": 16,
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
