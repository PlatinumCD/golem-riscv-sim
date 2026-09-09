import os
from pathlib import Path

import sst


tile_count = int(os.environ["MITTENS_READY_SET_TILES"])
worker_count = int(os.environ["MITTENS_READY_SET_WORKERS"])
runtime_ready_set = (
    os.environ.get("MITTENS_READY_SET_RUNTIME", "0") == "1"
)
if tile_count not in (2, 4, 16):
    raise RuntimeError("MITTENS_READY_SET_TILES must be 2, 4, or 16")
if worker_count <= 0 or worker_count > tile_count:
    raise RuntimeError("ready-set worker count must be in [1, tile_count]")

qemu_path = os.environ["MITTENS_READY_SET_QEMU"]
elf_path = os.environ["MITTENS_READY_SET_ELF"]
output_root = Path(os.environ["MITTENS_READY_SET_OUTPUT"])
uart_root = output_root / "uart"
profile_root = output_root / "performance"
uart_root.mkdir(parents=True)
profile_root.mkdir(parents=True)

proof = os.environ.get("MITTENS_READY_SET_PROOF", "")
if worker_count > 1:
    if len(proof) != 64 or any(
        digit not in "0123456789abcdef" for digit in proof
    ):
        raise RuntimeError("parallel ready-set test requires a SHA-256 proof")

initialization_quantum = int(
    os.environ["MITTENS_READY_SET_INITIALIZATION_QUANTUM"]
)
runtime_quantum = int(os.environ["MITTENS_READY_SET_RUNTIME_QUANTUM"])
analog_first = os.environ.get("MITTENS_READY_SET_ANALOG_FIRST", "0") == "1"

initialization_controller = sst.Component(
    "memory_init_barrier",
    "mittens.memoryInitializationBarrierController",
)
initialization_controller.addParams({
    "tile_count": tile_count,
    "active_tiles": list(range(tile_count)),
    "clock": "1GHz",
    "release_cycles": 1,
})

for tile_id in range(tile_count):
    tile = sst.Component(f"tile{tile_id}", "mittens.tile")
    params = {
        "tile_id": tile_id,
        "network_size": tile_count,
        "qemu_path": qemu_path,
        "elf": elf_path,
        "memory": "16M",
        "memory_backend": "streaming",
        "memory_init_batching": True,
        "memory_init_barrier_tiles": tile_count,
        "memory_init_instruction_quantum": initialization_quantum,
        # Streaming deployments enable the tile scratchpad, which also makes
        # QEMU's aggregate pre-runtime initialization phase active.  This
        # fixture performs no scratchpad access; it only needs the production
        # initialization protocol to be enabled.
        "scratchpad_enabled": True,
        # Production streaming guests enable scratchpad batching.  Their first
        # synchronization event is consequently the fence immediately before
        # the aggregate memory-initialization marker; keep the focused proof
        # on that exact boundary instead of a simplified configuration.
        "scratchpad_access_batching": True,
        "sync_instruction_quantum": runtime_quantum,
        "qemu_ready_set_workers": worker_count,
        "qemu_runtime_ready_set": runtime_ready_set,
        "launch_mode": "managed",
        "profile_mode": "summary",
        "profile_output_directory": str(profile_root),
        "serial_output_directory": str(uart_root),
        "progress_snapshot_interval_ms": 0,
        "progress_watchdog_ms": 30000,
        "verbose": 0,
    }
    if analog_first:
        params.update({
            "analog_array_count": 1,
            "analog_array_rows": 1,
            "analog_array_columns": 1,
            "analog_backend": "native",
        })
    if worker_count > 1:
        params["qemu_ready_set_independence_proof"] = proof
    tile.addParams(params)
    initialization_link = sst.Link(
        f"tile{tile_id}.memory_init_barrier"
    )
    initialization_link.connect(
        (tile, "memoryInitBarrier", "1ns"),
        (initialization_controller, f"barrier{tile_id}", "1ns"),
    )
