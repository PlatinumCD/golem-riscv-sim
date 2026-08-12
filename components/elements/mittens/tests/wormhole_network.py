import os
from pathlib import Path

import sst


WIDTH = 2
HEIGHT = 2
PORT_EAST = 0
PORT_WEST = 1
PORT_SOUTH = 2
PORT_NORTH = 3
PORT_LOCAL = 4
LINK_LATENCY = "1ns"
BUFFER_FLITS = 32
LINK_WIDTH_BITS = int(
    os.environ.get("MITTENS_WORMHOLE_LINK_WIDTH_BITS", "32")
)


def endpoint_id(x, y):
    return y * WIDTH + x


sst.setProgramOption("timebase", "1ps")
output_path = Path(os.environ["MITTENS_WORMHOLE_OUTPUT"])
statistics_path = Path(os.environ["MITTENS_WORMHOLE_STATS"])

routers = {}
for y in range(HEIGHT):
    for x in range(WIDTH):
        router = sst.Component(
            f"router_{x}_{y}",
            "mittens.wormholeRouter",
        )
        router.addParams(
            {
                "id": endpoint_id(x, y),
                "mesh_width": WIDTH,
                "mesh_height": HEIGHT,
                "clock": "1GHz",
                "link_width_bits": LINK_WIDTH_BITS,
                "input_buffer_flits": BUFFER_FLITS,
                "pipeline_cycles": 3,
            }
        )
        routers[x, y] = router

for y in range(HEIGHT):
    for x in range(WIDTH):
        router = routers[x, y]
        if x + 1 < WIDTH:
            link = sst.Link(f"horizontal_{x}_{y}")
            link.connect(
                (router, f"port{PORT_EAST}", LINK_LATENCY),
                (routers[x + 1, y], f"port{PORT_WEST}", LINK_LATENCY),
            )
        if y + 1 < HEIGHT:
            link = sst.Link(f"vertical_{x}_{y}")
            link.connect(
                (router, f"port{PORT_SOUTH}", LINK_LATENCY),
                (routers[x, y + 1], f"port{PORT_NORTH}", LINK_LATENCY),
            )

        node = endpoint_id(x, y)
        probe = sst.Component(
            f"probe{node}",
            "mittens.networkTimingProbe",
        )
        probe_params = {
            "endpoint_id": node,
            "network_size": WIDTH * HEIGHT,
            "payload_words": 16,
            "clock": "1GHz",
            "link_clock": "1GHz",
            "link_width_bits": LINK_WIDTH_BITS,
            "tail_delivery": True,
            "send_cycle": 100,
            "timeout_cycles": 1000,
        }
        if node in (1, 2):
            probe_params["destination"] = 3
        if node == 3:
            probe_params["expected_receives"] = 2
            probe_params["output_path"] = str(output_path)
        probe.addParams(probe_params)

        nic = probe.setSubComponent(
            "networkIF",
            "mittens.wormholeNIC",
        )
        nic.addParams(
            {
                "endpoint_id": node,
                "network_size": WIDTH * HEIGHT,
                "clock": "1GHz",
                "link_width_bits": LINK_WIDTH_BITS,
                "router_buffer_flits": BUFFER_FLITS,
                "injection_buffer_flits": 64,
            }
        )
        local = sst.Link(f"local_{node}")
        local.connect(
            (nic, "router_port", LINK_LATENCY),
            (router, f"port{PORT_LOCAL}", LINK_LATENCY),
        )
        local.setNoCut()

sst.setStatisticLoadLevel(1)
sst.setStatisticOutput(
    "sst.statOutputCSV",
    {"filepath": str(statistics_path), "separator": ","},
)
sst.enableAllStatisticsForAllComponents(
    {"type": "sst.AccumulatorStatistic", "rate": "0ns"}
)
