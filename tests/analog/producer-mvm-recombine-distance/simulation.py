import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1]))

from support.mesh import build_mesh


def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


width = 9
height = 9
producer = int(required("MITTENS_PMR_PRODUCER_TILE"))
mvm = int(required("MITTENS_PMR_MVM_TILE"))
recombine = int(required("MITTENS_PMR_RECOMBINE_TILE"))
image_directory = Path(required("MITTENS_PMR_IMAGE_DIRECTORY"))
idle_image = required("MITTENS_PMR_IDLE_ELF")

images = [idle_image] * (width * height)
for tile_id in {producer, mvm, recombine}:
    image = image_directory / f"tile-{tile_id}.elf"
    if not image.is_file():
        raise RuntimeError(f"missing active tile image: {image}")
    images[tile_id] = str(image)

sst.setProgramOption("timebase", "1ps")

build_mesh(
    width=width,
    height=height,
    qemu_path=required("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required("MITTENS_PMR_STATISTICS"),
    verbosity=int(os.environ.get("MITTENS_PMR_VERBOSITY", "0")),
    tile_params={
        "cpu_clock": "1GHz",
        "cpu_issue_width": 2,
        "sync_instruction_quantum": 1000000,
        "analog_array_count": 2,
        "analog_array_rows": 256,
        "analog_array_columns": 512,
        "analog_backend": "native",
        "analog_link_clock": "1GHz",
        "analog_compute_latency_cycles": 100,
        "rx_dma_clock": "1GHz",
        "rx_dma_width_bits": 256,
        "rx_dma_setup_cycles": 8,
        "rx_dma_queue_depth": 4,
        "profile_mode": "trace",
        "profile_output_directory": required(
            "MITTENS_PMR_PROFILE_DIRECTORY"
        ),
        "task_trace_directory": required(
            "MITTENS_PMR_TASK_DIRECTORY"
        ),
    },
    # One physical timing cell is exactly one architectural 32-bit word.
    # Buffer capacity is independent and remains large enough for a complete
    # 512-word activation frame.
    network_cell_words=1,
    network_buffer_cells=16384,
    mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
    memory_backend="native",
)
