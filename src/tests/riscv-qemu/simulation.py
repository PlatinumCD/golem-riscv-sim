"""A real RISC-V guest and an independent StandardMem peer share one SPM."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_riscv, resolve

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
p = resolve(case.get("parameters"), cpu_parameters=case.get("cpu_parameters"))
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "11ms")
clients = []
if not case.get("expected_failure"):
    peer = sst.Component("peer", "tilecomponents.RiscvPeer")
    peer.addParam("spm_request_bytes", p["spm_request_bytes"])
    clients.append(peer.setSubComponent("memory", "memHierarchy.standardInterface"))
cpu, scratch = connect_riscv(
    sst, p, elf=case["elf"], qemu=case["qemu"],
    memory_file=str(trial / "scratchpad.bin"),
    cpu_parameters=case.get("cpu_parameters"), clients=clients)
