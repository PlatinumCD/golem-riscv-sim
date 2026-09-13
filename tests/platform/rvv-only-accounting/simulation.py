import os

import sst

profile_directory = os.environ["MITTENS_TEST_PROFILE"]
task_directory = os.environ["MITTENS_TEST_TASKS"]

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": os.environ["MITTENS_TEST_QEMU"],
        "elf": os.environ["MITTENS_TEST_ELF"],
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "cpu_issue_width": 1,
        "sync_instruction_quantum": 1000,
        "riscv_vector_enabled": True,
        "riscv_vector_length_bits": 256,
        "riscv_vector_element_bits": 64,
        "profile_mode": "trace",
        "profile_output_directory": profile_directory,
        "task_trace_directory": task_directory,
        "verbose": 0,
    }
)

# Program loading and data DMA use the same shared controller.
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support"))
from spm import attach_memory
attach_memory({0: tile})
