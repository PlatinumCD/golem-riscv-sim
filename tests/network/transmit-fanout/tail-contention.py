import os
import sys
from pathlib import Path

import sst

TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import build_mesh

def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value

source_count = int(required("MITTENS_TAIL_CONTENTION_SOURCES"))
elf_directory = Path(required("MITTENS_FANOUT_ELF_DIRECTORY"))
elf_prefix = os.environ.get(
    "MITTENS_FANOUT_ELF_PREFIX", f"tail-contention-{source_count}"
)
sst.setProgramOption("timebase", "1ps")

build_mesh(
    width=source_count + 1,
    height=1,
    qemu_path=required("MITTENS_TEST_QEMU"),
    images=[
        str(
            elf_directory /
            f"{elf_prefix}-tile{tile}.elf"
        )
        for tile in range(source_count + 1)
    ],
    statistics_path=required("MITTENS_FANOUT_STATS"),
    verbosity=0,
    tile_params={
        "memory": "16M",
        "cpu_clock": "1GHz",
        "cpu_issue_width": 1,
        "sync_instruction_quantum": 1000,
        "profile_mode": "trace",
        "profile_output_directory": required("MITTENS_FANOUT_PROFILE"),
        "task_trace_directory": required("MITTENS_FANOUT_TASK_TRACE"),
        "rx_dma_streaming": os.environ.get("MITTENS_TEST_RX_STREAMING", "0") == "1",
        "rx_dma_streams": int(os.environ.get("MITTENS_TEST_RX_STREAMS", "1")),
        "rx_dma_width_bits": 256,
        "rx_dma_setup_cycles": int(
            os.environ.get("MITTENS_FANOUT_RX_DMA_SETUP_CYCLES", "8")
        ),
        "rx_dma_queue_depth": 4,

    },
    memory_backend="streaming",
    mesh_router_backend="mittens",
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
    mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
)
