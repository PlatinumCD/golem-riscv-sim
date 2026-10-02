"""One real DRAM control CPU distributes weights to two real compute tiles."""
import json
import os
from pathlib import Path
import sys
import sst

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE))
from components.mordred.tiles import connect_riscv_mesh

trial = Path(os.environ['TILE_COMPONENT_OUTPUT'])
case = json.loads((trial/'case.json').read_text())
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '2ms')
network = connect_riscv_mesh(sst, case['parameters'], elfs=case['elfs'],
    memory_directory=trial, qemu=case['qemu'], cpu_parameters=case['cpu_parameters'],
    mesh_parameters=dict(x_dim=2, y_dim=2), router_parameters=case['router_parameters'],
    network_transfers=case['transfers'], dram_tiles={0:case['dram']}, name='net')
sst.setStatisticLoadLevel(7)
sst.setStatisticOutput('sst.statOutputCSV', {'filepath':str(trial/'statistics.csv')})
sst.enableAllStatisticsForAllComponents({'rate':'0ns'})
