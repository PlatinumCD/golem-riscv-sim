import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parent / "support"))

from mesh import build_mesh

sst.setProgramOption("timebase", "1ps")

backend = os.environ.get("MITTENS_C3_MEMORY_BACKEND", "native")
profile = os.environ.get("MITTENS_C3_PROFILE", "")
params = {
    "memory": "64M",
    "cpu_clock": "1GHz",
    "cpu_issue_width": 2,
    "sync_instruction_quantum": 1000000,
    "analog_array_count": 4,
    "analog_array_rows": 1024,
    "analog_array_columns": 512,
    "analog_backend": "native",
    "analog_link_clock": "1GHz",
    "analog_compute_latency_cycles": 100,
}
if profile:
    params.update(
        {
            "profile_mode": "trace",
            "profile_output_directory": profile,
        }
    )
if backend == "memhierarchy":
    params.update(
        {
            "memory_init_batching": True,
            "memory_init_bytes_per_cycle": 32,
            "memory_init_latency_cycles": 2,
        }
    )

build_mesh(
    width=1,
    height=1,
    qemu_path=os.environ["MITTENS_TEST_QEMU"],
    images=[os.environ["MITTENS_C3_ELF"]],
    statistics_path=os.environ["MITTENS_C3_STATS"],
    verbosity=0,
    network_cell_words=1,
    network_buffer_cells=16384,
    memory_backend=backend,
    memory_hierarchy={"topology": "shared_l2", "l2_banks": 2},
    tile_params=params,
)
