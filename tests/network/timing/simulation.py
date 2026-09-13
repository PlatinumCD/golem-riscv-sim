import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import (  # noqa: E402
    LINK_LATENCY,
    LOCAL_PORT,
    _connect_routers,
    _make_wormhole_router,
    _mesh_link_configuration,
    tile_id,
)


def required(name):
    value = os.environ.get(name)
    if value is None or not value:
        raise RuntimeError(f"{name} must be set")
    return value


width = int(required("MITTENS_NETWORK_TIMING_WIDTH"))
height = int(required("MITTENS_NETWORK_TIMING_HEIGHT"))
network_size = width * height
sources = {
    int(value)
    for value in required("MITTENS_NETWORK_TIMING_SOURCES").split(",")
}
destination = int(required("MITTENS_NETWORK_TIMING_DESTINATION"))
payload_words = int(required("MITTENS_NETWORK_TIMING_PAYLOAD_WORDS"))
output_path = required("MITTENS_NETWORK_TIMING_OUTPUT")
statistics_path = required("MITTENS_NETWORK_TIMING_STATS")

if width <= 0 or height <= 0:
    raise RuntimeError("mesh dimensions must be positive")
if not sources or any(source < 0 or source >= network_size for source in sources):
    raise RuntimeError("network timing source is outside the mesh")
if destination < 0 or destination >= network_size:
    raise RuntimeError("network timing destination is outside the mesh")
if destination in sources:
    raise RuntimeError("network timing destination must not be a source")
if payload_words <= 0:
    raise RuntimeError("network timing payload must be positive")

sst.setProgramOption("timebase", "1ps")

link_bandwidth, flit_size, buffer_size = _mesh_link_configuration(
    32,
    "1GHz",
    1,
    4096,
)
routers = {
    (x, y): _make_wormhole_router(x, y, width, height, "1GHz", 32, 4096, 3, 1)
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
                0,
                routers[x + 1, y],
                1,
            )
        if y + 1 < height:
            _connect_routers(
                f"vertical_{x}_{y}_to_{x}_{y + 1}",
                router,
                2,
                routers[x, y + 1],
                3,
            )

        endpoint = tile_id(x, y, width)
        probe = sst.Component(
            f"probe{endpoint}",
            "mittens.networkTimingProbe",
        )
        params = {
            "endpoint_id": endpoint,
            "network_size": network_size,
            "payload_words": payload_words,
            "clock": "1GHz",
            "link_clock": "1GHz",
            "link_width_bits": 32,
            "tail_delivery": True,
            "send_cycle": 100,
            "timeout_cycles": 100000,
        }
        if endpoint in sources:
            params["destination"] = destination
        if endpoint == destination:
            params["expected_receives"] = len(sources)
            params["output_path"] = output_path
        probe.addParams(params)

        network = probe.setSubComponent(
            "networkIF",
            "mittens.wormholeNIC",
        )
        network.addParams(
            {
                "endpoint_id": endpoint, "network_size": network_size,
                "mesh_width": width, "mesh_height": height, "clock": "1GHz",
                "link_width_bits": 32, "router_buffer_flits": 4096,
                "injection_buffer_flits": 4096,
            }
        )
        local = sst.Link(f"probe{endpoint}_local_link")
        local.connect(
            (network, "router_port0", LINK_LATENCY),
            (router, f"port{LOCAL_PORT}", LINK_LATENCY),
        )
        local.setNoCut()

sst.setStatisticLoadLevel(1)
sst.setStatisticOutput(
    "sst.statOutputCSV",
    {"filepath": statistics_path, "separator": ","},
)
sst.enableAllStatisticsForAllComponents(
    {"type": "sst.AccumulatorStatistic", "rate": "0ns"}
)
