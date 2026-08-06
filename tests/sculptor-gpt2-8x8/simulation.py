import os
import sys
from pathlib import Path

import sst


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parent / "support"))

from mesh import build_mesh

sst.setProgramOption("timebase", "1ps")


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


active_core_ids = [
    int(value)
    for value in required_path("MITTENS_GPT2_ACTIVE_CORE_IDS").split(",")
]
mesh_width = int(os.environ.get("MITTENS_GPT2_MESH_WIDTH", "8"))
mesh_height = int(os.environ.get("MITTENS_GPT2_MESH_HEIGHT", "8"))
if mesh_width <= 0 or mesh_height <= 0:
    raise RuntimeError("GPT-2 mesh dimensions must be positive")
network_size = mesh_width * mesh_height
if not active_core_ids:
    raise RuntimeError("MITTENS_GPT2_ACTIVE_CORE_IDS must not be empty")
if len(set(active_core_ids)) != len(active_core_ids):
    raise RuntimeError("MITTENS_GPT2_ACTIVE_CORE_IDS contains duplicates")
if any(core_id < 0 or core_id >= network_size for core_id in active_core_ids):
    raise RuntimeError(
        "MITTENS_GPT2_ACTIVE_CORE_IDS contains a core outside the configured "
        f"{mesh_width}x{mesh_height} mesh"
    )

idle_image = required_path("MITTENS_GPT2_IDLE_ELF")
images = [idle_image] * network_size
for core_id in active_core_ids:
    images[core_id] = required_path(f"MITTENS_GPT2_CORE{core_id}_ELF")

tile_params = {
    "memory": "64M",
    "cpu_clock": os.environ.get("MITTENS_GPT2_CPU_CLOCK", "1GHz"),
    "cpu_issue_width": int(
        os.environ.get("MITTENS_GPT2_CPU_ISSUE_WIDTH", "1")
    ),
    "sync_instruction_quantum": int(
        os.environ.get("MITTENS_GPT2_SYNC_QUANTUM", "1000000")
    ),
    "rx_dma_clock": os.environ.get("MITTENS_GPT2_RX_DMA_CLOCK", "1GHz"),
    "rx_dma_width_bits": int(
        os.environ.get("MITTENS_GPT2_RX_DMA_WIDTH_BITS", "256")
    ),
    "rx_dma_setup_cycles": int(
        os.environ.get("MITTENS_GPT2_RX_DMA_SETUP_CYCLES", "8")
    ),
    "rx_dma_queue_depth": int(
        os.environ.get("MITTENS_GPT2_RX_DMA_QUEUE_DEPTH", "4")
    ),
    "analog_array_count": 4,
    "analog_array_rows": 1024,
    "analog_array_columns": 512,
    "analog_backend": "native",
    "analog_link_clock": "1GHz",
    "analog_compute_latency_cycles": int(
        os.environ.get(
            "MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES",
            "100",
        )
    ),
    "scratchpad_enabled": os.environ.get(
        "MITTENS_GPT2_SCRATCHPAD_ENABLED", "0"
    ) == "1",
    "scratchpad_bytes": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_BYTES", "262144")
    ),
    "scratchpad_banks": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_BANKS", "8")
    ),
    "scratchpad_read_ports": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_READ_PORTS", "1")
    ),
    "scratchpad_write_ports": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_WRITE_PORTS", "1")
    ),
    "scratchpad_access_width_bits": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_WIDTH_BITS", "256")
    ),
    "scratchpad_latency_cycles": int(
        os.environ.get("MITTENS_GPT2_SCRATCHPAD_LATENCY_CYCLES", "1")
    ),
    "scratchpad_dma_bytes_per_cycle": int(
        os.environ.get(
            "MITTENS_GPT2_SCRATCHPAD_DMA_BYTES_PER_CYCLE", "32"
        )
    ),
    "scratchpad_dma_setup_cycles": int(
        os.environ.get(
            "MITTENS_GPT2_SCRATCHPAD_DMA_SETUP_CYCLES", "8"
        )
    ),
}
profile_mode = os.environ.get("MITTENS_GPT2_PROFILE_MODE", "off")
if profile_mode != "off":
    tile_params.update(
        {
            "profile_mode": profile_mode,
            "profile_output_directory": required_path(
                "MITTENS_GPT2_PROFILE_RAW_DIRECTORY"
            ),
        }
    )
memory_backend = os.environ.get("MITTENS_MEMORY_BACKEND", "native")
memory_topology = os.environ.get(
    "MITTENS_MEMORY_TOPOLOGY", "private_l1"
)
if memory_backend == "memhierarchy":
    tile_params.update(
        {
            "memory_init_batching": True,
            "memory_init_bytes_per_cycle": int(
                os.environ.get(
                    "MITTENS_GPT2_MEMORY_INIT_BYTES_PER_CYCLE",
                    "32",
                )
            ),
            "memory_init_latency_cycles": int(
                os.environ.get(
                    "MITTENS_GPT2_MEMORY_INIT_LATENCY_CYCLES",
                    "2",
                )
            ),
            "memory_store_buffer_entries": int(
                os.environ.get(
                    "MITTENS_MEMORY_STORE_BUFFER_ENTRIES", "1"
                )
            ),
        }
    )

build_mesh(
    width=mesh_width,
    height=mesh_height,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=images,
    statistics_path=required_path("MITTENS_GPT2_STATS"),
    verbosity=int(os.environ.get("MITTENS_GPT2_VERBOSITY", "0")),
    # Preserve 64 KiB of buffering while modeling one physical 32-bit word
    # per timing cell instead of padding each request to a 16 KiB cell.
    network_cell_words=1,
    network_buffer_cells=16384,
    mesh_link_width_bits=int(
        os.environ.get("MITTENS_GPT2_MESH_LINK_WIDTH_BITS", "32")
    ),
    mesh_link_clock=os.environ.get(
        "MITTENS_GPT2_MESH_LINK_CLOCK", "1GHz"
    ),
    memory_backend=memory_backend,
    memory_hierarchy={
        "topology": memory_topology,
        "l2_banks": int(
            os.environ.get("MITTENS_MEMORY_L2_BANKS", "2")
        ),
        "l2_slice_size": os.environ.get(
            "MITTENS_MEMORY_L2_SLICE_SIZE", "512KiB"
        ),
        "l2_associativity": int(
            os.environ.get("MITTENS_MEMORY_L2_ASSOCIATIVITY", "8")
        ),
        "l2_access_latency_cycles": int(
            os.environ.get("MITTENS_MEMORY_L2_LATENCY_CYCLES", "10")
        ),
        "memory_network_bandwidth": os.environ.get(
            "MITTENS_MEMORY_NETWORK_BANDWIDTH", "64GB/s"
        ),
    },
    tile_params=tile_params,
)
