import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parent / "support"))

from mesh import build_mesh

sst.setProgramOption("timebase", "1ps")


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


active_images = [
    required_path(f"MITTENS_RESNET18_CORE{core_id}_ELF")
    for core_id in range(19)
]
idle_image = required_path("MITTENS_RESNET18_IDLE_ELF")
images = active_images + [idle_image] * (64 - len(active_images))
sync_instruction_quantum = int(
    os.environ.get("MITTENS_RESNET18_SYNC_QUANTUM", "1000000")
)
task_trace_directory = os.environ.get(
    "MITTENS_RESNET18_TASK_TRACE_RAW_DIRECTORY", ""
)
tile_params = {
    "memory": "64M",
    "cpu_clock": os.environ.get(
        "MITTENS_RESNET18_CPU_CLOCK", "1GHz"
    ),
    "sync_instruction_quantum": sync_instruction_quantum,
    "rx_dma_clock": os.environ.get(
        "MITTENS_RESNET18_RX_DMA_CLOCK", "1GHz"
    ),
    "rx_dma_width_bits": int(
        os.environ.get("MITTENS_RESNET18_RX_DMA_WIDTH_BITS", "256")
    ),
    "rx_dma_setup_cycles": int(
        os.environ.get("MITTENS_RESNET18_RX_DMA_SETUP_CYCLES", "8")
    ),
    "rx_dma_queue_depth": int(
        os.environ.get("MITTENS_RESNET18_RX_DMA_QUEUE_DEPTH", "4")
    ),
    "analog_array_count": 4,
    "analog_array_rows": 1024,
    "analog_array_columns": 512,
    "analog_backend": "native",
    "analog_link_clock": "1GHz",
    "analog_compute_latency_cycles": 8,
}
if task_trace_directory:
    tile_params["task_trace_directory"] = task_trace_directory

build_mesh(
    width=8,
    height=8,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required_path("MITTENS_RESNET18_STATS"),
    verbosity=int(os.environ.get("MITTENS_RESNET18_VERBOSITY", "0")),
    network_cell_words=4096,
    network_buffer_cells=4,
    mesh_link_width_bits=int(
        os.environ.get("MITTENS_RESNET18_MESH_LINK_WIDTH_BITS", "32")
    ),
    mesh_link_clock=os.environ.get(
        "MITTENS_RESNET18_MESH_LINK_CLOCK", "1GHz"
    ),
    tile_params=tile_params,
)
