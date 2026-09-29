"""Two independent request interfaces connected to overlapping physical banks."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_scratchpad

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "2us")
driver = sst.Component("driver", "tilecomponents.BankConnectionTest")
driver.addParam("scenario", case["name"])
clients = [driver.setSubComponent(role, "memHierarchy.standardInterface") for role in ("cpu", "router", "unknown")]
p = dict(spm_capacity_bytes=4096, spm_banks=4, spm_bank_width=4,
         cpu_spm_banks=[0, 2], router_spm_banks=[1, 2],
         spm_write_ports_per_bank=2 if case["name"] == "shared-two-ports" else 1)
if case["name"] == "visibility-router-one":
    p.update(cpu_spm_banks=[0, 1, 2, 3], router_spm_banks=[2])
elif case["name"] == "visibility-router-three":
    p.update(cpu_spm_banks=[2], router_spm_banks=[1, 2, 3])
scratch = connect_scratchpad(sst, p, clients,
    cpu_requestor="driver:cpu", router_requestor="driver:router")
backing = trial / "spm.bin"
backing.write_bytes(b"\x11" * 4096)
scratch.addParams(dict(backing="mmap", memory_file=str(backing)))
