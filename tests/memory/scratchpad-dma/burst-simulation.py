import os
import sys
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import build_mesh

# Every endpoint submits the QEMU contract maximum of eight requests before
# waiting.  Seventeen endpoints therefore produce a legal 136-request burst,
# exercising both the full per-endpoint bound and automatic total-queue
# scaling beyond the historical fixed depth of 16.
endpoint_count = 17
image = os.environ["MITTENS_TEST_ELF"]
build_mesh(
    width=endpoint_count,
    height=1,
    qemu_path=os.environ["MITTENS_TEST_QEMU"],
    images=[image] * endpoint_count,
    statistics_path=os.environ["MITTENS_BURST_STATS"],
    verbosity=0,
    tile_params={
        "qemu_control_memory": "16M",

        "scratchpad_enabled": True,
        "scratchpad_bytes": 262144,
        "serial_output_directory": os.environ["MITTENS_BURST_SERIAL"],

    },
    memory_backend="streaming",
    global_memory={
        "channels": 1,
        "fixed_latency_cycles": 100000,
    },
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
)
