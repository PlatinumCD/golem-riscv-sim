"""Two real guest CPUs initiate and consume NIU messages without a test controller."""
import json
import os
from pathlib import Path
import sys
import sst

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from components.mordred.tiles import connect_riscv_mesh

trial = Path(os.environ['TILE_COMPONENT_OUTPUT'])
case = json.loads((trial/'case.json').read_text())
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '2ms')
network = connect_riscv_mesh(sst, case['parameters'], elfs=case['elfs'],
    memory_directory=trial, qemu=case['qemu'], cpu_parameters=case['cpu_parameters'],
    mesh_parameters=case.get('mesh_parameters',{}) | dict(x_dim=case.get("mesh_x",2), y_dim=case.get("mesh_y",1), link_latency=case.get("link_latency","1ns"), flit_size_bits=case.get('flit_bits',128), nic_output_buffer_bytes=case.get('nic_bytes',4096), num_vcs=case.get('vcs', 1)),
    router_parameters=case['router_parameters'], network_transfers=case['network_transfers'], name='net')

sst.setStatisticLoadLevel(7)
sst.setStatisticOutput('sst.statOutputCSV', {'filepath':str(trial/'network-statistics.csv')})
sst.enableAllStatisticsForAllComponents({'rate':'0ns'})
