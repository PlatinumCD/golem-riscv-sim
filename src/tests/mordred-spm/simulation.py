"""A 2x2 mesh of real QEMU/RVV, shared bank, network and analog-array tiles."""
import json
import os
from pathlib import Path
import sys
import sst

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from components.mordred.tiles import connect_riscv_mesh

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
case = json.loads((trial / "case.json").read_text())
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "2ms")
network = connect_riscv_mesh(sst, case.get("parameters", {}), elfs=case["elfs"],
    qemu=case["qemu"], memory_directory=trial, cpu_parameters=case.get("cpu_parameters", {}),
    mesh_parameters=case.get("mesh_parameters", {}), router_parameters=case.get("router_parameters", {}))
sst.setStatisticLoadLevel(7)
sst.setStatisticOutput("sst.statOutputCSV", {"filepath": str(trial / "network-statistics.csv")})
sst.enableAllStatisticsForAllComponents({"rate": "0ns"})
# Save the composition's backing filenames rather than infer them in validators.
(trial / "tiles.json").write_text(json.dumps([
    dict(id=i, memory_file=str(tile["memory_file"])) for i, tile in enumerate(network["tiles"])], indent=2) + "\n")

# Test-only initiators submit explicit remote transactions. Production endpoints
# remain passive and never inspect guest coordination data independently.
for identity, tile in enumerate(network["tiles"]):
    client = sst.Component(f"tile_mesh.client{identity}", "tilecomponents.SharedBankInitiator")
    client.addParams(dict(tile_id=identity, banks=case["parameters"].get("spm_banks", 4),
        bank_width=case["parameters"].get("spm_bank_width", 4),
        router_bank0=case["parameters"]["router_spm_banks"][0],
        router_bank1=case["parameters"]["router_spm_banks"][1]))
    sst.Link(f"tile_mesh.client{identity}_requests").connect(
        (client, "requests", "1ns"), (tile["router_spm"], "requests", "1ns"))
