import os

import sst


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": required_path("MITTENS_TEST_QEMU"),
        "elf": required_path("MITTENS_TEST_ELF"),
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "cpu_issue_width": int(
            os.environ.get("MITTENS_TEST_CPU_ISSUE_WIDTH", "1")
        ),
        "riscv_vector_enabled": True,
        "riscv_vector_length_bits": 256,
        "riscv_vector_element_bits": 64,
        "verbose": 2,
    }
)
