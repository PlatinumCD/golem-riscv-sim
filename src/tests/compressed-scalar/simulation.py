"""One real CPU, one banked SPM and one array; no peer component required."""
import json
import os
from pathlib import Path
import sys

import sst

trial = Path(os.environ['TILE_COMPONENT_OUTPUT'])
case = json.loads((trial / 'case.json').read_text())
sys.path.insert(0, case['component_source'])
from configuration import connect_riscv_arrays

sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '10ms')
connect_riscv_arrays(sst, case['parameters'], elf=case['elf'], qemu=case['qemu'],
                     memory_file=trial / 'scratchpad.bin',
                     cpu_parameters=case['cpu_parameters'])
