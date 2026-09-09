import os
import sys
from pathlib import Path

import sst

TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[3] / "tests/support"))
from mesh import build_mesh


def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


pattern = int(os.environ.get("MITTENS_FANOUT_PATTERN", "0"))
active_by_pattern = {
    0: (7, 11, 12, 13, 17),
    1: (10, 11, 12, 13, 14),
    2: (2, 7, 12, 13, 14),
    3: (7, 11, 12, 14, 22),
    4: (1, 3, 5, 6, 8, 9, 15, 16, 18, 19, 21, 23),
    5: (7, 12, 13),
    6: (12, 13),
    7: (7, 11, 12, 13),
    8: (12, 13),
    9: (7, 12, 13),
    10: (7, 12, 13),
    11: (7, 12, 13, 17),
    12: (7, 11, 12, 13, 17),
}
if pattern not in active_by_pattern:
    raise RuntimeError("MITTENS_FANOUT_PATTERN must be in [0,12]")
active = active_by_pattern[pattern]
tx_streams = int(os.environ.get("MITTENS_TX_STREAMS", "1"))
payload_bytes = int(os.environ.get("MITTENS_FANOUT_PAYLOAD_BYTES", "4096"))
wire_words = 7 + payload_bytes // 4
if tx_streams not in (1, 2, 4):
    raise RuntimeError("MITTENS_TX_STREAMS must be 1, 2, or 4")
elf_dir = Path(required("MITTENS_TX_FANOUT_ELF_DIR"))
images = [""] * 25
for tile in active:
    images[tile] = str(elf_dir / f"tile{tile}.elf")

sst.setProgramOption("timebase", "1ps")
build_mesh(
    width=5, height=5, qemu_path=required("MITTENS_TEST_QEMU"),
    images=images, active_tiles=active,
    statistics_path=required("MITTENS_TX_FANOUT_STATS"), verbosity=0,
    tile_params={
        "memory": "16M", "cpu_clock": "1GHz", "cpu_issue_width": 1,
        "sync_instruction_quantum": 1000,
        "scratchpad_enabled": True,
        "scratchpad_bytes": int(os.environ.get(
            "MITTENS_TX_FANOUT_SPM_BYTES", "16384")),
        "scratchpad_banks": 8, "scratchpad_read_ports": 1,
        "scratchpad_write_ports": 1, "scratchpad_access_width_bits": 256,
        "scratchpad_latency_cycles": 1, "scratchpad_access_batching": True,
        "scratchpad_access_run_compaction": True,
        "rx_dma_width_bits": 256, "rx_dma_setup_cycles": 8,
        "rx_dma_queue_depth": 4, "memory_init_batching": True,
        "tx_dma_streams": tx_streams,
        "tx_dma_fifo_bytes": int(os.environ.get(
            "MITTENS_TX_DMA_FIFO_BYTES", "128")),
        "memory_init_instruction_quantum": 1000000,
        "memory_init_bytes_per_cycle": 32, "memory_init_latency_cycles": 2,
        "profile_mode": "summary",
        "profile_output_directory": required("MITTENS_TX_FANOUT_PROFILE"),
        "task_trace_directory": required("MITTENS_TX_FANOUT_TASKS"),
        "serial_output_directory": required("MITTENS_TX_FANOUT_SERIAL"),
    },
    memory_backend="streaming", mesh_router_backend="mittens",
    network_cell_words=1, network_buffer_cells=max(16384, wire_words * 2),
    network_packet_words=wire_words, mesh_link_width_bits=32,
    mesh_link_clock="1GHz",
    wormhole_input_buffer_flits=max(8192, wire_words * 2),
    wormhole_injection_buffer_flits=max(8192, wire_words * 2),
)
