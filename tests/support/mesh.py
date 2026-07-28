from decimal import Decimal, InvalidOperation
import re

import sst


EAST_PORT = 0
WEST_PORT = 1
SOUTH_PORT = 2
NORTH_PORT = 3
LOCAL_PORT = 4
ROUTER_PORTS = 5

LINK_LATENCY = "10ns"
WORD_BYTES = 4
WORD_BITS = WORD_BYTES * 8

_FREQUENCY_PATTERN = re.compile(
    r"^([0-9]+(?:\.[0-9]+)?)\s*(Hz|kHz|MHz|GHz)$",
    re.IGNORECASE,
)
_FREQUENCY_SCALES = {
    "hz": Decimal(1),
    "khz": Decimal(1_000),
    "mhz": Decimal(1_000_000),
    "ghz": Decimal(1_000_000_000),
}


def tile_id(x, y, width):
    return y * width + x


def _mesh_link_configuration(width_bits, clock, cell_words, buffer_cells):
    if (isinstance(width_bits, bool) or
            not isinstance(width_bits, int) or
            width_bits <= 0 or
            width_bits % WORD_BITS != 0):
        raise ValueError(
            "mesh link width must be a positive multiple of 32 bits"
        )
    if not isinstance(clock, str):
        raise ValueError("mesh link clock must be an SST frequency string")

    match = _FREQUENCY_PATTERN.fullmatch(clock.strip())
    if match is None:
        raise ValueError(
            "mesh link clock must use Hz, kHz, MHz, or GHz"
        )
    try:
        frequency_hz = (
            Decimal(match.group(1)) *
            _FREQUENCY_SCALES[match.group(2).lower()]
        )
    except InvalidOperation as error:
        raise ValueError("invalid mesh link clock") from error
    if frequency_hz <= 0:
        raise ValueError("mesh link clock must be positive")

    bytes_per_second = frequency_hz * Decimal(width_bits) / Decimal(8)
    if bytes_per_second != bytes_per_second.to_integral_value():
        raise ValueError(
            "mesh width and clock must produce an integral byte rate"
        )

    link_bytes = width_bits // 8
    useful_cell_bytes = cell_words * WORD_BYTES
    flit_bytes = (
        (useful_cell_bytes + link_bytes - 1) // link_bytes
    ) * link_bytes
    return (
        f"{int(bytes_per_second)}B/s",
        f"{flit_bytes}B",
        f"{flit_bytes * buffer_cells}B",
    )


def _make_router(x, y, width, height, flit_size, buffer_size,
                 link_bandwidth):
    router_id = tile_id(x, y, width)
    router = sst.Component(f"router_{x}_{y}", "merlin.hr_router")
    router.addParams(
        {
            "id": router_id,
            "num_ports": ROUTER_PORTS,
            "flit_size": flit_size,
            "xbar_bw": link_bandwidth,
            "link_bw": link_bandwidth,
            "input_buf_size": buffer_size,
            "output_buf_size": buffer_size,
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


def _attach_tile(router, node_id, image, qemu_path, network_size, verbosity,
                 tile_params, buffer_size, link_bandwidth):
    tile = sst.Component(f"tile{node_id}", "mittens.tile")
    params = {
        "tile_id": node_id,
        "network_size": network_size,
        "qemu_path": qemu_path,
        "elf": image,
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "verbose": verbosity,
    }
    params.update(tile_params)
    tile.addParams(params)

    network = tile.setSubComponent("networkIF", "merlin.linkcontrol")
    network.addParams(
        {
            "link_bw": link_bandwidth,
            "input_buf_size": buffer_size,
            "output_buf_size": buffer_size,
        }
    )

    link = sst.Link(f"tile{node_id}_local_link")
    link.connect(
        (network, "rtr_port", LINK_LATENCY),
        (router, f"port{LOCAL_PORT}", LINK_LATENCY),
    )
    link.setNoCut()


def build_mesh(*, width, height, qemu_path, images, statistics_path,
               verbosity=2, tile_params=None, network_cell_words=1,
               network_buffer_cells=16, mesh_link_width_bits=32,
               mesh_link_clock="1GHz"):
    """Build a mesh whose physical links carry fixed 32-bit words.

    ``mesh_link_width_bits`` controls how many of those words a link can move
    per ``mesh_link_clock`` cycle. Timing cells are padded to a complete
    physical beat; neither setting changes the guest-visible NIC word size.
    """
    network_size = width * height
    if len(images) != network_size:
        raise ValueError(
            f"mesh requires {network_size} images, received {len(images)}"
        )
    if tile_params is None:
        tile_params = {}
    if network_cell_words <= 0 or network_buffer_cells <= 0:
        raise ValueError(
            "network cell and buffer counts must both be positive"
        )
    link_bandwidth, flit_size, buffer_size = _mesh_link_configuration(
        mesh_link_width_bits,
        mesh_link_clock,
        network_cell_words,
        network_buffer_cells,
    )

    routers = {
        (x, y): _make_router(
            x,
            y,
            width,
            height,
            flit_size,
            buffer_size,
            link_bandwidth,
        )
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
                tile_params,
                buffer_size,
                link_bandwidth,
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
