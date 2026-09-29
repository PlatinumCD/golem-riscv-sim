"""Real QEMU core, banked SPM and independent memory observer."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_riscv, connect_riscv_arrays

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "10ms")
clients = []
if case.get("peer", True):
    peer = sst.Component("peer", "tilecomponents.RiscvPeer")
    peer.addParams(dict(spm_request_bytes=case["parameters"]["spm_request_bytes"], observe_vector_store=True))
    clients.append(peer.setSubComponent("memory", "memHierarchy.standardInterface"))
connect = connect_riscv_arrays if case.get("mode") == 5 else connect_riscv
connect(sst, case["parameters"], elf=case["elf"], qemu=case["qemu"],
        memory_file=trial / "scratchpad.bin", cpu_parameters=case["cpu_parameters"], clients=clients)
