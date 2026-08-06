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
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "verbose": 1,
    }
)
