"""One tile and a deliberately fast RAM controller; no network or analog work."""
import json
import os
from pathlib import Path

import sst

trial = Path(os.environ["GLOBAL_DMA_CLOCK_TRIAL"])
cpu_clock = os.environ["GLOBAL_DMA_CLOCK_CPU"]
setup_cpu_cycles = int(os.environ["GLOBAL_DMA_CLOCK_SPM_SETUP"])

# Force an explicit common unit for all recorded SST ticks (one picosecond).
sst.setProgramOption("timebase", "1ps")
tile_params = {
    "tile_id": 0,
    "qemu_path": os.environ["GLOBAL_DMA_CLOCK_QEMU"],
    "elf": os.environ["GLOBAL_DMA_CLOCK_ELF"],
    "memory": "16M",
    "global_ram_bytes": 1048576,
    "memory_backend": "streaming",
    "memory_init_batching": True,
    "memory_init_bytes_per_cycle": 32,
    "memory_init_latency_cycles": 2,
    "memory_init_instruction_quantum": 64 * 1024 * 1024,
    "scratchpad_enabled": True,
    "scratchpad_bytes": 262144,
    "scratchpad_banks": 8,
    "scratchpad_access_width_bits": 256,
    "scratchpad_latency_cycles": 1,
    "scratchpad_dma_bytes_per_cycle": 32,
    "scratchpad_dma_setup_cycles": setup_cpu_cycles,
    # SPM-enabled configurations require matching RX/SPM setup even without NIC.
    "rx_dma_setup_cycles": setup_cpu_cycles,
    # Exercise WAIT and WAIT_BATCH themselves without transport fusion.
    "global_dma_submit_batching": False,
    "global_dma_macro_execution": False,
    "launch_mode": "managed",
    "cpu_clock": cpu_clock,
    "sync_instruction_quantum": 1000000,
    "task_trace_directory": str(trial / "tasks"),
    "serial_output_directory": str(trial / "serial"),
    "profile_mode": "trace",
    "profile_output_directory": str(trial / "profile"),
    "verbose": 1,
}
ram_params = {
    "tile_count": 1,
    "capacity_bytes": 1048576,
    "clock": "1GHz",
    "channels": 1,
    "queue_depth": 8,
    "per_tile_queue_depth": 8,
    "setup_cycles": 0,
    "bytes_per_cycle": 4096,
    "burst_bytes": 4096,
    "fixed_latency_cycles": int(os.environ.get("GLOBAL_DMA_CLOCK_RAM_LATENCY", "0")),
    "profile_output_directory": str(trial / "profile"),
}
tile = sst.Component("tile0", "mittens.tile")
tile.addParams(tile_params)
ram = sst.Component("global_ram", "mittens.globalRAMController")
ram.addParams(ram_params)
link = sst.Link("tile0.global_dma")
link.connect((tile, "globalDMA", "1ns"), (ram, "dma0", "1ns"))
link.setNoCut()
(trial / "parameters.json").write_text(json.dumps({
    "timebase": "1ps", "tile": tile_params, "global_ram": ram_params,
    "link_latency_ticks": 1000,
}, indent=2) + "\n")
