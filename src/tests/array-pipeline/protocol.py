"""Direct command traffic tests queue backpressure without CPU serialization."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_arrays

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "1ms")
driver = sst.Component("protocol", "tilecomponents.PipelineProtocol")
driver.addParam("execute_cycles", case["parameters"]["cost_per_mvm_cycles"])
arrays = connect_arrays(sst, case["parameters"])
sst.Link("commands").connect((driver, "commands", "1ns"), (arrays, "commands", "1ns"))
