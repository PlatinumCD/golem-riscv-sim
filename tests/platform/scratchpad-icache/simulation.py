import os

import sst

def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value

tile = sst.Component("tile0", "mittens.tile")
tile.addParams({
    "tile_id": 0,
    "qemu_path": required("MITTENS_TEST_QEMU"),
    "elf": required("MITTENS_TEST_ELF"),
    "memory": "16M",
    "launch_mode": "managed",
    "cpu_clock": "1GHz",
    "cpu_issue_width": 1,
    "sync_instruction_quantum": int(os.environ.get("MITTENS_TEST_QUANTUM", "1")),
    "memory_backend": "streaming",
    "global_ram_bytes": 16 * 1024 * 1024,
    "memory_access_batching": False,

    "memory_event_batching": False,
    # Transaction assembly must not depend on the cross-instruction batch limit.
    "qemu_ready_set_workers": 1,
    "qemu_runtime_ready_set": False,
    "qemu_local_lookahead": False,
    "scratchpad_boot": True,
    "scratchpad_enabled": True,
    "instruction_cache_bytes": 8192,
    "instruction_cache_line_bytes": 64,
    "instruction_cache_ways": 2,
    "instruction_cache_hit_cycles": 1,
    "profile_mode": "trace",
    "profile_output_directory": required("MITTENS_TEST_PROFILE"),
    "verbose": 1,
})

controller = sst.Component("global_ram", "mittens.globalRAMController")
controller.addParams({
    "tile_count": 1,
    "active_tiles": [0],
    "capacity_bytes": 16 * 1024 * 1024,
    "clock": "1GHz",
    "channels": 1,
    "queue_depth": 8,
    "per_tile_queue_depth": 8,
    "setup_cycles": 8,
    "bytes_per_cycle": 32,
    "burst_bytes": 64,
    "fixed_latency_cycles": 2,
})

dma = sst.Link("tile0.global_dma")
dma.connect((tile, "globalDMA", "1ns"), (controller, "dma0", "1ns"))
dma.setNoCut()
