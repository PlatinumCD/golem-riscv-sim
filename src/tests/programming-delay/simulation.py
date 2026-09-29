"""Array programming timing fixtures with either direct commands or real RVV."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_arrays, connect_riscv_arrays

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
p = case["parameters"]
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "10ms")
if case["kind"] == "protocol":
    arrays = connect_arrays(sst, p)
    if case.get("invalid_scope"):
        arrays.addParam("array_program_delay_scope", "invalid")
    driver = sst.Component("protocol", "tilecomponents.ProgrammingDelayProtocol")
    driver.addParams(dict(initial=p["array_program_delay_scope"] == "initial_full_array",
                          pipeline=p["array_pipeline_enabled"], arrays=p["arrays_per_tile"],
                          delay=p["cost_per_array_program_cycles"]))
    sst.Link("commands").connect((driver, "commands", "1ns"), (arrays, "commands", "1ns"))
else:
    connect_riscv_arrays(sst, p, elf=case["elf"], qemu=case.get("qemu"), memory_file=trial / "scratchpad.bin",
        cpu_parameters=dict(load_store_queue_depth=16, host_timeout_seconds=60))
