import os
from pathlib import Path

import sst


invalid_mode = os.environ.get(
    "MITTENS_MEMORY_INIT_BARRIER_INVALID", "none"
)
tile_count = 4
active_tiles = [0, 2, 3]
arrival_delays = {0: 1, 2: 7, 3: 3}
thread_count = sst.getThreadCount()
profile_directory = Path(
    os.environ["MITTENS_MEMORY_INIT_BARRIER_PROFILE_DIR"]
)
statistics_path = Path(
    os.environ["MITTENS_MEMORY_INIT_BARRIER_STATS"]
)

controller = sst.Component(
    "memory_init_barrier",
    "mittens.memoryInitializationBarrierController",
)
controller.addParams({
    "tile_count": tile_count,
    "active_tiles": active_tiles,
    "clock": "1GHz",
    "release_cycles": 4,
    "release_tiles_per_cycle": 1,
    "profile_output_directory": str(profile_directory),
    "verbose": 1,
})
controller.setRank(0, 0)

for index, tile_id in enumerate(active_tiles):
    probe = sst.Component(
        f"memory_init_probe_{tile_id}",
        "mittens.memoryInitializationBarrierProbe",
    )
    probe.addParams({
        "tile_id": tile_id,
        "arrival_delay": arrival_delays[tile_id],
        "invalid_mode": (
            invalid_mode if tile_id == active_tiles[0] else "none"
        ),
        "clock": "1GHz",
    })
    if thread_count > 1:
        probe.setRank(0, (index + 1) % thread_count)
    else:
        probe.setRank(0, 0)

    link = sst.Link(f"memory_init_barrier_tile_{tile_id}")
    link.connect(
        (probe, "barrier", "1ns"),
        (controller, f"barrier{tile_id}", "1ns"),
    )

sst.setStatisticLoadLevel(1)
sst.setStatisticOutput(
    "sst.statOutputCSV",
    {"filepath": str(statistics_path), "separator": ","},
)
sst.enableAllStatisticsForComponentName("memory_init_barrier")
