import os
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from deployment_manifest import load_deployment_manifest
from mesh import build_mesh


deployment = load_deployment_manifest(
    os.environ["MITTENS_EPOCH_BARRIER_DEPLOYMENT_MANIFEST"],
    network_size=2,
    expected_active_tiles=[0, 1],
)
if deployment["synchronization_mode"] != "bulk_barrier":
    raise RuntimeError("epoch-barrier test requires bulk_barrier mode")


build_mesh(
    width=2,
    height=1,
    qemu_path=os.environ["MITTENS_TEST_QEMU"],
    images=[
        os.environ["MITTENS_EPOCH_BARRIER_TILE0"],
        os.environ["MITTENS_EPOCH_BARRIER_TILE1"],
    ],
    statistics_path=os.environ["MITTENS_EPOCH_BARRIER_STATS"],
    verbosity=2,
    tile_params={
        "qemu_control_memory": "16M",
        "scratchpad_enabled": True,
        "scratchpad_bytes": 2 * 1024 * 1024,
        "serial_output_directory": os.environ["MITTENS_EPOCH_BARRIER_SERIAL"],
    },
    memory_backend="streaming",
    global_memory={
        "dependency_mode": "bulk_barrier",
        "channels": 1,
        "queue_depth": 8,
        "per_tile_queue_depth": 4,
        "setup_cycles": 8,
        "bytes_per_cycle": 32,
        "burst_bytes": 64,
        "fixed_latency_cycles": 2,
    },
    epoch_barrier={
        "epoch_count": deployment["epoch_count"],
        "clock": "1GHz",
        "release_cycles": 4,
        "verbose": 2,
    },
    network_cell_words=1,
    network_buffer_cells=16,
    network_packet_words=16,
    mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
    active_tiles=deployment["active_tile_ids"],
)
