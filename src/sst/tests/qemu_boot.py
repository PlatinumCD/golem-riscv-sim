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
ram = sst.Component("global_ram", "mittens.globalRAMController")
ram.addParams({"tile_count": 1, "active_tiles": [0],
               "dependency_mode": "bulk_barrier"})
dma = sst.Link("tile0.global_dma")
dma.connect((tile, "globalDMA", "1ns"), (ram, "dma0", "1ns"))
dma.setNoCut()
if os.environ.get("MITTENS_PROVENANCE_PROFILE"):
    tile.addParams({"profile_mode": "summary",
                    "profile_output_directory": os.environ["MITTENS_PROVENANCE_PROFILE"]})
