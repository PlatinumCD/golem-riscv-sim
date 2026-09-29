"""Coalesced QEMU vector accesses use the actual shared banked SPM."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_riscv

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "10ms")
peer = sst.Component("peer", "tilecomponents.RiscvPeer")
peer.addParam("spm_request_bytes", case["parameters"]["spm_request_bytes"])
peer.addParam("observe_vector_store", True)
interface = peer.setSubComponent("memory", "memHierarchy.standardInterface")
connect_riscv(sst, case["parameters"], elf=case["elf"], qemu=case["qemu"],
    memory_file=trial / "scratchpad.bin", clients=[interface])
