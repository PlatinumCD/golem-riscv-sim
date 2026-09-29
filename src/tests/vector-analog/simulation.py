"""CPU-driven vector-register transfers with analog arrays and shared SPM."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_riscv_arrays

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "11ms")
connect_riscv_arrays(sst, case["parameters"], elf=case["elf"], qemu=case["qemu"],
    memory_file=trial / "scratchpad.bin", cpu_parameters=case.get("cpu_parameters"))
