import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from support.mesh import build_mesh


def required(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


def environment(name):
    return os.environ[name]


def integer(name):
    return int(environment(name))


def boolean(name):
    value = environment(name).strip().lower()
    if value in ("1", "true", "yes", "on"):
        return True
    if value in ("0", "false", "no", "off"):
        return False
    raise ValueError(f"{name} must be true or false")


packet_words = os.environ.get("MITTENS_SCULPTOR_NETWORK_PACKET_WORDS", "")
memory_backend = os.environ.get("MITTENS_SCULPTOR_MEMORY_BACKEND", "native")


build_mesh(
    width=integer("MITTENS_SCULPTOR_MESH_COLS"),
    height=integer("MITTENS_SCULPTOR_MESH_ROWS"),
    qemu_path=required("MITTENS_TEST_QEMU"),
    images=[
        str(Path(required("MITTENS_SCULPTOR_ELF_DIRECTORY")) / f"tile-{tile}.elf")
        for tile in range(
            integer("MITTENS_SCULPTOR_MESH_ROWS") *
            integer("MITTENS_SCULPTOR_MESH_COLS")
        )
    ],
    statistics_path=required("MITTENS_SCULPTOR_STATS"),
    verbosity=int(os.environ.get("MITTENS_SCULPTOR_VERBOSITY", "0")),
    tile_params={
        "analog_array_count": integer("MITTENS_ANALOG_ARRAY_COUNT"),
        "analog_array_rows": integer("MITTENS_ANALOG_ARRAY_ROWS"),
        "analog_array_columns": integer("MITTENS_ANALOG_ARRAY_COLUMNS"),
        "analog_backend": environment("MITTENS_SCULPTOR_ANALOG_BACKEND"),
        "analog_link_clock": environment("MITTENS_SCULPTOR_ANALOG_LINK_CLOCK"),
        "analog_compute_latency_cycles": integer("MITTENS_SCULPTOR_ANALOG_COMPUTE_LATENCY_CYCLES"),
        "cpu_clock": environment("MITTENS_SCULPTOR_CPU_CLOCK"),
        "cpu_issue_width": integer("MITTENS_SCULPTOR_CPU_ISSUE_WIDTH"),
        "sync_instruction_quantum": integer("MITTENS_SCULPTOR_SYNC_INSTRUCTION_QUANTUM"),
        "riscv_vector_enabled": environment("MITTENS_SCULPTOR_RVV_ENABLED"),
        "riscv_vector_length_bits": integer("MITTENS_SCULPTOR_RVV_LENGTH_BITS"),
        "riscv_vector_element_bits": integer("MITTENS_SCULPTOR_RVV_ELEMENT_BITS"),
        "scratchpad_enabled": environment("MITTENS_SCULPTOR_SCRATCHPAD_ENABLED"),
        "profile_mode": environment("MITTENS_SCULPTOR_PROFILE_MODE"),
        "profile_output_directory": environment("MITTENS_SCULPTOR_PROFILE_OUTPUT_DIRECTORY"),
        "task_trace_directory": environment("MITTENS_SCULPTOR_TASK_TRACE_DIRECTORY"),
        "rx_dma_width_bits": integer("MITTENS_SCULPTOR_RX_DMA_WIDTH_BITS"),
        "rx_dma_setup_cycles": integer("MITTENS_SCULPTOR_RX_DMA_SETUP_CYCLES"),
        "rx_dma_queue_depth": integer("MITTENS_SCULPTOR_RX_DMA_QUEUE_DEPTH"),
        "memory": environment("MITTENS_SCULPTOR_TILE_MEMORY"),
        "memory_init_batching": (
            memory_backend == "memhierarchy" and
            boolean("MITTENS_SCULPTOR_MEMORY_INIT_BATCHING")
        ),
        "memory_init_bytes_per_cycle": integer(
            "MITTENS_SCULPTOR_MEMORY_INIT_BYTES_PER_CYCLE"
        ),
        "memory_init_latency_cycles": integer(
            "MITTENS_SCULPTOR_MEMORY_INIT_LATENCY_CYCLES"
        ),
    },
    memory_backend=memory_backend,
    mesh_link_width_bits=integer("MITTENS_SCULPTOR_MESH_LINK_WIDTH_BITS"),
    mesh_link_clock=environment("MITTENS_SCULPTOR_MESH_LINK_CLOCK"),
    network_cell_words=integer("MITTENS_SCULPTOR_NETWORK_CELL_WORDS"),
    network_buffer_cells=integer("MITTENS_SCULPTOR_NETWORK_BUFFER_CELLS"),
    network_packet_words=int(packet_words) if packet_words else None,
    memory_hierarchy={
        "l1_size": environment("MITTENS_SCULPTOR_L1_SIZE"),
        "l1_associativity": integer("MITTENS_SCULPTOR_L1_ASSOCIATIVITY"),
        "cache_line_size": integer("MITTENS_SCULPTOR_CACHE_LINE_SIZE"),
        "l1_access_latency_cycles": integer("MITTENS_SCULPTOR_L1_ACCESS_LATENCY_CYCLES"),
        "l1_clock": environment("MITTENS_SCULPTOR_L1_CLOCK"),
        "lower_memory_clock": environment("MITTENS_SCULPTOR_LOWER_MEMORY_CLOCK"),
        "lower_memory_access_time": environment("MITTENS_SCULPTOR_LOWER_MEMORY_ACCESS_TIME"),
    },
)
