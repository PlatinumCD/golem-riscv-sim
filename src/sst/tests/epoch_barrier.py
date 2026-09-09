import os

import sst


invalid_mode = os.environ.get("MITTENS_EPOCH_BARRIER_INVALID", "none")
stop_after_releases = int(
    os.environ.get("MITTENS_EPOCH_BARRIER_STOP_AFTER_RELEASES", "0")
)
thread_count = sst.getThreadCount()

if invalid_mode == "none":
    tile_count = 4
    active_tiles = [0, 2, 3]
    epoch_count = 3
    delays = {
        0: [1, 2, 1],
        2: [3, 1, 4],
        3: [2, 5, 2],
    }
    idle_epochs = {
        0: [1],
        2: [0, 2],
        3: [],
    }
else:
    tile_count = 1
    active_tiles = [0]
    epoch_count = 2 if invalid_mode == "stale" else 1
    delays = {0: [1] * epoch_count}
    idle_epochs = {0: []}

controller = sst.Component(
    "epoch_barrier", "mittens.epochBarrierController"
)
controller.addParams(
    {
        "tile_count": tile_count,
        "active_tiles": active_tiles,
        "epoch_count": epoch_count,
        "clock": "1GHz",
        "release_cycles": 4,
        "stop_after_releases": stop_after_releases,
        "verbose": 1,
    }
)
controller.setRank(0, 0)

for index, tile_id in enumerate(active_tiles):
    probe = sst.Component(
        f"epoch_probe_{tile_id}", "mittens.epochBarrierProbe"
    )
    probe.addParams(
        {
            "tile_id": tile_id,
            "epoch_count": epoch_count,
            "arrival_delays": delays[tile_id],
            "idle_epochs": idle_epochs[tile_id],
            "invalid_mode": invalid_mode,
            "clock": "1GHz",
        }
    )
    if thread_count > 1:
        probe.setRank(0, (index + 1) % thread_count)
    else:
        probe.setRank(0, 0)
    link = sst.Link(f"epoch_barrier_tile_{tile_id}")
    link.connect(
        (probe, "barrier", "1ns"),
        (controller, f"barrier{tile_id}", "1ns"),
    )

sst.setStatisticLoadLevel(1)
sst.setStatisticOutput("sst.statOutputConsole")
sst.enableAllStatisticsForComponentName("epoch_barrier")
