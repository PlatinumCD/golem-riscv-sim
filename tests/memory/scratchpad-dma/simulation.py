import os

import sst

profile_directory = os.environ.get("MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY", "")

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": os.environ["MITTENS_TEST_QEMU"],
        "elf": os.environ["MITTENS_TEST_ELF"],
        "memory": "16M",
        "memory_backend": "streaming",

        "scratchpad_enabled": True,
        "scratchpad_bytes": 262144,
        "scratchpad_banks": 8,
        "scratchpad_access_width_bits": 256,
        "scratchpad_dma_bytes_per_cycle": 32,
        "scratchpad_dma_setup_cycles": 8,

        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "sync_instruction_quantum": int(
            os.environ.get("MITTENS_TEST_SYNC_INSTRUCTION_QUANTUM", "1000")
        ),
        "profile_mode": "trace" if profile_directory else "off",
        "profile_output_directory": profile_directory,
        "verbose": int(os.environ.get("MITTENS_TEST_VERBOSE", "1")),
    }
)

controller = sst.Component(
    "global_ram", "mittens.globalRAMController"
)
controller.addParams(
    {
        "tile_count": 1,
        "capacity_bytes": 32 * 1024 ** 3,
        "clock": "1GHz",
        "channels": 1,
        "queue_depth": 8,
        "per_tile_queue_depth": 8,
        "setup_cycles": 8,
        "bytes_per_cycle": 32,
        "burst_bytes": 64,
        "fixed_latency_cycles": 2,
        "profile_output_directory": profile_directory,
    }
)
dma = sst.Link("tile0.global_dma")
dma.connect((tile, "globalDMA", "1ns"), (controller, "dma0", "1ns"))
dma.setNoCut()

statistics_path = os.environ.get("MITTENS_TEST_STATISTICS", "")
if statistics_path:
    sst.setStatisticLoadLevel(1)
    sst.setStatisticOutput(
        "sst.statOutputCSV",
        {"filepath": statistics_path, "separator": ","},
    )
    sst.enableAllStatisticsForAllComponents(
        {"type": "sst.AccumulatorStatistic", "rate": "0ns"}
    )
