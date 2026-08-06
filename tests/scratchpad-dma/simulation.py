import os

import sst


tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": os.environ["MITTENS_TEST_QEMU"],
        "elf": os.environ["MITTENS_TEST_ELF"],
        "memory": "16M",
        "memory_backend": "native",
        "scratchpad_enabled": True,
        "scratchpad_bytes": 262144,
        "scratchpad_banks": 8,
        "scratchpad_access_width_bits": 256,
        "scratchpad_dma_bytes_per_cycle": 32,
        "scratchpad_dma_setup_cycles": 8,
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "verbose": 1,
    }
)
