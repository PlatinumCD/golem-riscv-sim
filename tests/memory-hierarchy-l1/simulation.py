import os

import sst


def required_path(name):
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} must be set")
    return value


memory_backend = os.environ.get("MITTENS_MEMORY_BACKEND", "native")
memory_init_batching = (
    os.environ.get("MITTENS_MEMORY_INIT_BATCHING", "0") == "1"
)
if memory_backend not in ("native", "memhierarchy"):
    raise RuntimeError(
        "MITTENS_MEMORY_BACKEND must be native or memhierarchy"
    )

tile = sst.Component("tile0", "mittens.tile")
tile.addParams(
    {
        "tile_id": 0,
        "qemu_path": required_path("MITTENS_TEST_QEMU"),
        "elf": required_path("MITTENS_TEST_ELF"),
        "memory": "16M",
        "memory_backend": memory_backend,
        "memory_init_batching": memory_init_batching,
        "memory_init_bytes_per_cycle": 32,
        "memory_init_latency_cycles": 2,
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "sync_instruction_quantum": 1000,
        "verbose": 1,
    }
)

if memory_backend == "memhierarchy":
    memory_if = tile.setSubComponent(
        "memoryIF", "memHierarchy.standardInterface"
    )

    l1 = sst.Component("tile0.private_l1", "memHierarchy.Cache")
    l1.addParams(
        {
            "access_latency_cycles": 2,
            "cache_frequency": "1GHz",
            "replacement_policy": "lru",
            "coherence_protocol": "MESI",
            "associativity": 4,
            "cache_line_size": 64,
            "cache_size": "32KiB",
            "L1": 1,
        }
    )

    memory_controller = sst.Component(
        "tile0.memory_controller", "memHierarchy.MemController"
    )
    memory_controller.addParams(
        {
            "clock": "1GHz",
            "backing": "none",
            # Keep absolute RISC-V physical addresses in the timing model.
            # A nonzero controller base rebases responses to local offsets.
            "addr_range_start": 0,
            "addr_range_end": 0x80FFFFFF,
        }
    )
    memory = memory_controller.setSubComponent(
        "backend", "memHierarchy.simpleMem"
    )
    memory.addParams(
        {
            "access_time": "50ns",
            # simpleMem is timing-only and does not allocate this capacity.
            "mem_size": "2064MiB",
        }
    )

    cpu_l1 = sst.Link("tile0.cpu_l1")
    cpu_l1.connect(
        (memory_if, "lowlink", "1ns"),
        (l1, "highlink", "1ns"),
    )
    l1_memory = sst.Link("tile0.l1_memory")
    l1_memory.connect(
        (l1, "lowlink", "1ns"),
        (memory_controller, "highlink", "1ns"),
    )

    sst.setStatisticLoadLevel(7)
    sst.setStatisticOutput("sst.statOutputConsole")
    l1.enableStatistics(["CacheHits", "CacheMisses"])
