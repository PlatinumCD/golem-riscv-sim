import csv
import os
from pathlib import Path

import sst


scenario = os.environ.get("MITTENS_GLOBAL_RAM_READINESS_SCENARIO", "coverage")
stats_path = Path(os.environ["MITTENS_GLOBAL_RAM_READINESS_STATS"])
endpoint_count = 1 if scenario in (
    "demand", "priority", "reservation", "duplicate"
) else 2
addressable_tile_count = 4

controller = sst.Component("global_ram", "mittens.globalRAMController")
controller.addParams({
    "capacity_bytes": 1024 * 1024,
    "clock": "1GHz",
    "tile_count": addressable_tile_count,
    "active_tiles": list(range(endpoint_count)),
    "channels": 2 if scenario == "reservation" else 1,
    "queue_depth": 16,
    "per_tile_queue_depth": 8,
    "setup_cycles": 1,
    "bytes_per_cycle": 4096,
    "burst_bytes": 4096,
    "fixed_latency_cycles": 1,
    "dependency_mode": "exact_dependencies",
    "demand_write_burst": 8 if scenario == "demand" else 0,
    "read_priority_burst": 4 if scenario == "priority" else 0,
    "reserved_read_channels": 1 if scenario == "reservation" else 0,
    "profile_output_directory": str(stats_path.parent),
    "progress_snapshot_interval_ms": 0,
})

for tile in range(endpoint_count):
    probe = sst.Component(
        f"readiness_probe_{tile}", "mittens.globalRAMReadinessProbe")
    probe.addParams({"tile_id": tile, "scenario": scenario})
    link = sst.Link(f"readiness_link_{tile}")
    link.connect((probe, "ram", "1ns"), (controller, f"dma{tile}", "1ns"))

sst.setStatisticLoadLevel(1)
sst.setStatisticOutput(
    "sst.statOutputCSV",
    {"filepath": str(stats_path), "separator": ","},
)
sst.enableAllStatisticsForComponentName(
    "global_ram", {"type": "sst.AccumulatorStatistic", "rate": "0ns"})
