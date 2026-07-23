import sst


EAST_PORT = 0
WEST_PORT = 1
SOUTH_PORT = 2
NORTH_PORT = 3
LOCAL_PORT = 4
ROUTER_PORTS = 5

LINK_BANDWIDTH = "1GB/s"
LINK_LATENCY = "10ns"
BUFFER_SIZE = "64B"


def tile_id(x, y, width):
    return y * width + x


def _make_router(x, y, width, height):
    router_id = tile_id(x, y, width)
    router = sst.Component(f"router_{x}_{y}", "merlin.hr_router")
    router.addParams(
        {
            "id": router_id,
            "num_ports": ROUTER_PORTS,
            "flit_size": "4B",
            "xbar_bw": LINK_BANDWIDTH,
            "link_bw": LINK_BANDWIDTH,
            "input_buf_size": BUFFER_SIZE,
            "output_buf_size": BUFFER_SIZE,
        }
    )

    topology = router.setSubComponent("topology", "merlin.mesh")
    topology.addParams(
        {
            "shape": f"{width}x{height}",
            "width": "1x1",
            "local_ports": 1,
        }
    )
    return router


def _connect_routers(name, first, first_port, second, second_port):
    link = sst.Link(name)
    link.connect(
        (first, f"port{first_port}", LINK_LATENCY),
        (second, f"port{second_port}", LINK_LATENCY),
    )


def _attach_tile(router, node_id, image, qemu_path, network_size, verbosity):
    tile = sst.Component(f"tile{node_id}", "mittens.tile")
    tile.addParams(
        {
            "tile_id": node_id,
            "network_size": network_size,
            "qemu_path": qemu_path,
            "elf": image,
            "memory": "16M",
            "launch_mode": "managed",
            "process_poll_frequency": "1MHz",
            "verbose": verbosity,
        }
    )

    network = tile.setSubComponent("networkIF", "merlin.linkcontrol")
    network.addParams(
        {
            "link_bw": LINK_BANDWIDTH,
            "input_buf_size": BUFFER_SIZE,
            "output_buf_size": BUFFER_SIZE,
        }
    )

    link = sst.Link(f"tile{node_id}_local_link")
    link.connect(
        (network, "rtr_port", LINK_LATENCY),
        (router, f"port{LOCAL_PORT}", LINK_LATENCY),
    )
    link.setNoCut()


def build_mesh(*, width, height, qemu_path, images, statistics_path,
               verbosity=2):
    network_size = width * height
    if len(images) != network_size:
        raise ValueError(
            f"mesh requires {network_size} images, received {len(images)}"
        )

    routers = {
        (x, y): _make_router(x, y, width, height)
        for y in range(height)
        for x in range(width)
    }

    for y in range(height):
        for x in range(width):
            router = routers[x, y]

            if x + 1 < width:
                _connect_routers(
                    f"horizontal_{x}_{y}_to_{x + 1}_{y}",
                    router,
                    EAST_PORT,
                    routers[x + 1, y],
                    WEST_PORT,
                )

            if y + 1 < height:
                _connect_routers(
                    f"vertical_{x}_{y}_to_{x}_{y + 1}",
                    router,
                    SOUTH_PORT,
                    routers[x, y + 1],
                    NORTH_PORT,
                )

            node_id = tile_id(x, y, width)
            _attach_tile(
                router,
                node_id,
                images[node_id],
                qemu_path,
                network_size,
                verbosity,
            )

    sst.setStatisticLoadLevel(1)
    sst.setStatisticOutput(
        "sst.statOutputCSV",
        {"filepath": statistics_path, "separator": ","},
    )
    sst.enableAllStatisticsForAllComponents(
        {"type": "sst.AccumulatorStatistic", "rate": "0ns"}
    )

    return routers
