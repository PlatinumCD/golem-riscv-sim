import os
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from deployment_manifest import load_deployment_manifest
from mesh import build_mesh


def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


def positive_integer(name, default):
    value = int(os.environ.get(name, str(default)))
    if value <= 0:
        raise RuntimeError(f"{name} must be a positive integer")
    return value


width = int(required("MITTENS_MATERIALIZED_MESH_WIDTH"))
height = int(required("MITTENS_MATERIALIZED_MESH_HEIGHT"))
network_size = width * height
elf_dir = Path(required("MITTENS_MATERIALIZED_ELF_DIR"))
active_manifest = Path(required("MITTENS_MATERIALIZED_ACTIVE_CORES"))
active_tiles = [
    int(line.strip())
    for line in active_manifest.read_text(encoding="utf-8").splitlines()
    if line.strip()
]
deployment = load_deployment_manifest(
    required("MITTENS_MATERIALIZED_DEPLOYMENT_MANIFEST"),
    network_size=network_size,
    expected_active_tiles=active_tiles,
)
fallback = elf_dir / f"tile-{deployment['active_tile_ids'][0]}.elf"
images = [
    str(elf_dir / f"tile-{tile}.elf")
    if tile in deployment["active_tile_ids"]
    else str(fallback)
    for tile in range(network_size)
]


build_mesh(
    width=width,
    height=height,
    qemu_path=required("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required("MITTENS_MATERIALIZED_STATS"),
    verbosity=int(os.environ.get("MITTENS_MATERIALIZED_VERBOSITY", "1")),
    tile_params={
        "qemu_control_memory": "16M",
        "sync_instruction_quantum": positive_integer(
            "MITTENS_MATERIALIZED_SYNC_INSTRUCTION_QUANTUM", 1000
        ),
        "scratchpad_enabled": True,
        "scratchpad_bytes": 2 * 1024 * 1024,
        "memory_init_batching": True,
        "memory_init_bytes_per_cycle": 32,
        "memory_init_latency_cycles": 2,
        "progress_watchdog_ms": int(
            os.environ.get("MITTENS_MATERIALIZED_WATCHDOG_MS", "120000")
        ),
    },
    memory_backend="streaming",
    global_memory={
        "dependency_mode": deployment["synchronization_mode"],
        "channels": positive_integer(
            "MITTENS_MATERIALIZED_GLOBAL_RAM_CHANNELS", 1
        ),
        "queue_depth": max(16, len(active_tiles) * 8),
        "per_tile_queue_depth": 8,
        "setup_cycles": 8,
        "bytes_per_cycle": 32,
        "burst_bytes": 64,
        "fixed_latency_cycles": 2,
    },
    initialization_barrier={
        "clock": "1GHz",
        "release_cycles": 1,
        "verbose": 1,
    },
    epoch_barrier={
        "epoch_count": deployment["epoch_count"],
        "clock": "1GHz",
        "release_cycles": 4,
        "verbose": 1,
    } if deployment["synchronization_mode"] == "bulk_barrier" else None,
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
    mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
    active_tiles=deployment["active_tile_ids"],
)
