"""One RISC-V CPU and one banked SPM; no analog array component."""
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
sst.setProgramOption("stop-at", case.get("stop_at", "1s"))
connect_riscv(sst, case["parameters"], elf=case["elf"], qemu=case["qemu"],
    memory_file=trial / "scratchpad.bin", cpu_parameters=case["cpu_parameters"])
