import os

import sst


elf = os.environ.get("MITTENS_TEST_ELF")
if not elf:
    raise RuntimeError("MITTENS_TEST_ELF must name the bare-metal test ELF")

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": os.environ["MITTENS_TEST_QEMU"],
        "elf": elf,
        "memory": "16M",
        "launch_mode": "managed",
        "verbose": 2,
    }
)
