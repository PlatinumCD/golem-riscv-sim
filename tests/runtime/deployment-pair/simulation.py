import os
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from mesh import build_mesh


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


memory_backend = os.environ.get(
    "MITTENS_DEPLOYMENT_MEMORY_BACKEND", "native"
)
profile_directory = os.environ.get(
    "MITTENS_DEPLOYMENT_MEMORY_PROFILE", ""
)
tile_params = {
    "cpu_clock": os.environ.get("MITTENS_DEPLOYMENT_CPU_CLOCK", "1GHz"),
    "serial_output_directory": os.environ.get("MITTENS_DEPLOYMENT_SERIAL", ""),
    "scratchpad_enabled": os.environ.get(
        "MITTENS_DEPLOYMENT_SCRATCHPAD_ENABLED", "false"
    ).lower() in ("1", "true", "yes"),
    "rx_dma_clock": os.environ.get(
        "MITTENS_DEPLOYMENT_RX_DMA_CLOCK", "1GHz"
    ),
    "rx_dma_width_bits": int(
        os.environ.get("MITTENS_DEPLOYMENT_RX_DMA_WIDTH_BITS", "256")
    ),
    "rx_dma_setup_cycles": int(
        os.environ.get("MITTENS_DEPLOYMENT_RX_DMA_SETUP_CYCLES", "8")
    ),
    "rx_dma_queue_depth": int(
        os.environ.get("MITTENS_DEPLOYMENT_RX_DMA_QUEUE_DEPTH", "4")
    ),
}
if memory_backend == "memhierarchy":
    tile_params.update(
        {
            "memory_init_batching": True,
            "profile_mode": "trace" if profile_directory else "summary",
            "profile_output_directory": profile_directory,
        }
    )
barrier_tiles = os.environ.get("MITTENS_DEPLOYMENT_MEMORY_INIT_BARRIER_TILES", "")
initialization_barrier = None
if barrier_tiles:
    if int(barrier_tiles) != 2:
        raise RuntimeError(
            "deployment-pair initialization barrier must contain both tiles"
        )
    initialization_barrier = {
        "clock": "1GHz",
        "release_cycles": 1,
    }
initialization_quantum = os.environ.get(
    "MITTENS_DEPLOYMENT_MEMORY_INIT_INSTRUCTION_QUANTUM", ""
)
if initialization_quantum:
    tile_params["memory_init_instruction_quantum"] = int(
        initialization_quantum
    )
progress_watchdog_ms = os.environ.get(
    "MITTENS_DEPLOYMENT_PROGRESS_WATCHDOG_MS", ""
)
if progress_watchdog_ms:
    tile_params["progress_watchdog_ms"] = int(progress_watchdog_ms)

build_mesh(
    width=2,
    height=1,
    qemu_path=required_path("MITTENS_TEST_QEMU"),
    images=[
        required_path("MITTENS_DEPLOYMENT_TILE0_ELF"),
        required_path("MITTENS_DEPLOYMENT_TILE1_ELF"),
    ],
    statistics_path=required_path("MITTENS_DEPLOYMENT_STATS"),
    verbosity=int(
        os.environ.get("MITTENS_DEPLOYMENT_VERBOSITY", "3")
    ),
    tile_params=tile_params,
    # Use a small buffer to make sure that SST fragments the 300-word tensor.
    network_cell_words=1,
    network_buffer_cells=16,
    mesh_link_width_bits=int(
        os.environ.get("MITTENS_DEPLOYMENT_MESH_LINK_WIDTH_BITS", "32")
    ),
    mesh_link_clock=os.environ.get(
        "MITTENS_DEPLOYMENT_MESH_LINK_CLOCK", "1GHz"
    ),
    mesh_router_backend=os.environ.get(
        "MITTENS_DEPLOYMENT_MESH_ROUTER_BACKEND", "merlin"
    ),
    wormhole_input_buffer_flits=int(
        os.environ.get(
            "MITTENS_DEPLOYMENT_WORMHOLE_INPUT_BUFFER_FLITS", "32"
        )
    ),
    wormhole_injection_buffer_flits=int(
        os.environ.get(
            "MITTENS_DEPLOYMENT_WORMHOLE_INJECTION_BUFFER_FLITS", "64"
        )
    ),
    wormhole_pipeline_cycles=int(
        os.environ.get(
            "MITTENS_DEPLOYMENT_WORMHOLE_PIPELINE_CYCLES", "3"
        )
    ),
    memory_backend=memory_backend,
    memory_hierarchy={
        "topology": os.environ.get(
            "MITTENS_DEPLOYMENT_MEMORY_TOPOLOGY", "private_l1"
        ),
        "l2_banks": int(
            os.environ.get("MITTENS_DEPLOYMENT_L2_BANKS", "2")
        ),
    },
    initialization_barrier=initialization_barrier,
)
