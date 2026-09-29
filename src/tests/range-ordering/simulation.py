"""Controlled external CPU and independent peer on one shared banked SPM."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import _connect_scratchpad

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "2us")
backing = trial / "scratchpad.bin"
backing.write_bytes(bytes([0x11]) * case["parameters"]["spm_capacity_bytes"])
driver = sst.Component("driver", "tilecomponents.RangeOrderingProtocol")
driver.addParams(dict(scenario=case["name"], memory_file=str(backing)))
external = driver.setSubComponent("external", "memHierarchy.standardInterface")
peer = driver.setSubComponent("peer", "memHierarchy.standardInterface")
scratch = _connect_scratchpad(sst, case["parameters"], [external, peer], memory_file=backing,
                              external_write_requestor="driver:external")
sst.Link("external_spm_commit").connect((driver, "external_commit", "0ps"),
                                        (scratch, "external_commit", "0ps"))
