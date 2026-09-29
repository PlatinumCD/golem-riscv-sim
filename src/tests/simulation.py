"""Independent SPM probes and register-payload array fixtures."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from configuration import connect, resolve

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
p = resolve(case.get("parameters"))
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "11ms")
driver = sst.Component("test", "tilecomponents.Driver")
driver.addParams(dict(rows=p["array_rows"], cols=p["array_cols"], arrays=p["arrays_per_tile"],
                      spm_request_bytes=p["spm_request_bytes"], spm_bank_width=p["spm_bank_width"],
                      spm_banks=p["spm_banks"], chunk_elements=case.get("chunk_elements", 9),
                      scenario=case.get("scenario", "arrays")))
memory = driver.setSubComponent("memory", "memHierarchy.standardInterface")
arrays, scratch = connect(sst, p, [memory], experimental=case.get("experimental"))
# Negative fixtures deliberately bypass the public resolver to check the C++
# component's own VLEN/derived-width validation.
arrays.addParams(case.get("array_parameter_overrides", {}))
sst.Link("commands").connect((driver, "commands", "1ns"), (arrays, "commands", "1ns"))
