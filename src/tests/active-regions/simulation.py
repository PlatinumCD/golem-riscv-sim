"""Active extents tested through protocol commands or actual RVV instructions."""
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
    driver = sst.Component("protocol", "tilecomponents.ActiveRegionProtocol")
    driver.addParam("pipeline", p["array_pipeline_enabled"])
    sst.Link("commands").connect((driver, "commands", "1ns"), (arrays, "commands", "1ns"))
else:
    connect_riscv_arrays(sst, p, elf=case["elf"], qemu=case["qemu"], memory_file=trial / "scratchpad.bin",
        cpu_parameters=case["cpu_parameters"])
