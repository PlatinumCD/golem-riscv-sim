import os

import sst


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


qemu = required_path("MITTENS_TEST_QEMU")
images = [
    required_path("MITTENS_TILE0_ELF"),
    required_path("MITTENS_TILE1_ELF"),
]

router = sst.Component("mesh_router", "merlin.hr_router")
router.addParams(
    {
        "id": 0,
        "num_ports": 2,
        "flit_size": "8B",
        "xbar_bw": "1GB/s",
        "link_bw": "1GB/s",
        "input_buf_size": "64B",
        "output_buf_size": "64B",
    }
)
router.setSubComponent("topology", "merlin.singlerouter")

for tile_id, image in enumerate(images):
    tile = sst.Component(f"tile{tile_id}", "mittens.tile")
    tile.addParams(
        {
            "tile_id": tile_id,
            "network_size": len(images),
            "qemu_path": qemu,
            "elf": image,
            "memory": "16M",
            "launch_mode": "managed",
            "process_poll_frequency": "1MHz",
            "verbose": 2,
        }
    )

    network = tile.setSubComponent("networkIF", "merlin.linkcontrol")
    network.addParams(
        {
            "link_bw": "1GB/s",
            "input_buf_size": "64B",
            "output_buf_size": "64B",
        }
    )

    link = sst.Link(f"tile{tile_id}_network_link")
    link.connect(
        (network, "rtr_port", "10ns"),
        (router, f"port{tile_id}", "10ns"),
    )
    link.setNoCut()
