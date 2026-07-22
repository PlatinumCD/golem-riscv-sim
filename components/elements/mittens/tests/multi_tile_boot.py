import os

import sst


elf = os.environ.get("MITTENS_TEST_ELF")
if not elf:
    raise RuntimeError("MITTENS_TEST_ELF must name the bare-metal test ELF")

tile_count = int(os.environ.get("MITTENS_TEST_TILES", "4"))
if tile_count < 2:
    raise RuntimeError("MITTENS_TEST_TILES must be at least 2")

qemu_path = os.environ["MITTENS_TEST_QEMU"]

for tile_id in range(tile_count):
    tile = sst.Component(f"tile{tile_id}", "mittens.tile")
    tile.addParams(
        {
            "tile_id": tile_id,
            "qemu_path": qemu_path,
            "elf": elf,
            "memory": "16M",
            "launch_mode": "managed",
            "verbose": 2,
        }
    )
