"""Exercise the public mesh helper without a test-specific partitioner."""
import json
import os
from pathlib import Path
import sys
import sst

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE))
from components.mordred.tiles import connect_riscv_mesh

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "2ms")
network = connect_riscv_mesh(sst, case["parameters"], elfs=case["elfs"],
    memory_directory=trial, qemu=case["qemu"], cpu_parameters=case["cpu_parameters"],
    mesh_parameters=dict(x_dim=case["mesh_x"], y_dim=case["mesh_y"]),
    router_parameters=case["router_parameters"],
    network_transfers=case.get("network_transfers", case.get("transfers")),
    dram_tiles={0: case["dram"]} if "dram" in case else None,
    tile_threads=case.get("tile_threads"), name="net")
if case.get("reject_unplaced_cpu"):
    network["tiles"][0]["cpu"].addParams(dict(sst_tile_thread=-1))
(trial / "tile-threads.json").write_text(json.dumps(network["tile_threads"]) + "\n")
sst.setStatisticLoadLevel(7)
sst.setStatisticOutput("sst.statOutputCSV", {"filepath": str(trial / "network-statistics.csv")})
sst.enableAllStatisticsForAllComponents({"rate": "0ns"})
