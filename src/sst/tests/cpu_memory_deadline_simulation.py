"""Cacheless R5b fixture: ordinary StandardMem plus private SPM, no NIC."""
import json
import os
from pathlib import Path
import sst

trial = Path(os.environ["CPU_DEADLINE_TRIAL"])
sst.setProgramOption("timebase", "1ps")
params = {
    "tile_id": 0, "qemu_path": os.environ["CPU_DEADLINE_QEMU"],
    "elf": os.environ["CPU_DEADLINE_ELF"], "memory": "16M",
    "global_ram_bytes": 1048576, "memory_backend": "memhierarchy",
    "memory_store_buffer_entries": int(os.environ["CPU_DEADLINE_STORES"]),
    "memory_init_batching": True, "memory_init_instruction_quantum": 67108864,
    "memory_access_batching": False, "scratchpad_access_batching": False,
    "scratchpad_enabled": True, "scratchpad_bytes": 262144,
    "scratchpad_latency_cycles": 10000,
    "launch_mode": "managed", "cpu_clock": os.environ["CPU_DEADLINE_CPU"],
    "cpu_issue_width": 1, "sync_instruction_quantum": 1000000,
    "task_trace_directory": str(trial / "tasks"),
    "serial_output_directory": str(trial / "serial"),
    "profile_mode": "trace", "profile_output_directory": str(trial / "profile"),
    "verbose": 3,
}
tile = sst.Component("tile0", "mittens.tile")
tile.addParams(params)
interface = tile.setSubComponent("memoryIF", "memHierarchy.standardInterface")
interface.addParams({"noncacheable_regions": [0, 0xFFFFFFFF]})
ram = sst.Component("ram", "memHierarchy.MemController")
ram.addParams({"clock": "1GHz", "backing": "none", "addr_range_start": 0,
               "addr_range_end": 0xFFFFFFFF})
backend = ram.setSubComponent("backend", "memHierarchy.simpleMem")
backend.addParams({"access_time": os.environ.get("CPU_DEADLINE_RAM", "100ns"),
                   "mem_size": "4096MiB"})
link = sst.Link("cacheless")
link.connect((interface, "lowlink", "1ns"), (ram, "highlink", "1ns"))
link.setNoCut()
(trial / "parameters.json").write_text(json.dumps(params, indent=2) + "\n")
