"""One router/SPM interface, explicit self-target requests, and no CPU."""
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_scratchpad
from components.mordred.configuration import connect_mesh

trial = Path(os.environ["TILE_COMPONENT_OUTPUT"])
sst.setProgramOption("timebase", "1ps")
sst.setProgramOption("stop-at", "10us")
endpoint = sst.Component("local.router_spm", "tilecomponents.MordredSpmEndpoint")
endpoint.addParams(dict(tile_id=0, tile_count=1, spm_capacity_bytes=4096,
    spm_request_bytes=4, spm_banks=4, spm_bank_width=4, router_spm_banks=[2, 3],
    request_window=4, memory_queue_depth=2))
memory = endpoint.setSubComponent("memory", "memHierarchy.standardInterface")
scratch = connect_scratchpad(sst, dict(spm_capacity_bytes=4096, spm_request_bytes=4,
    spm_banks=4, spm_bank_width=4, router_spm_banks=[2, 3]), [memory],
    name_prefix="local.", router_requestor="local.router_spm:memory")
backing = trial / "spm.bin"
backing.write_bytes(b"\x11" * 4096)
scratch.addParams(dict(backing="mmap", memory_file=str(backing)))
connect_mesh(sst, dict(x_dim=1, y_dim=1), name="fabric", endpoints=[endpoint])
driver = sst.Component("driver", "tilecomponents.MordredLocalTest")
sst.Link("local_requests").connect((driver, "requests", "1ns"), (endpoint, "requests", "1ns"))
sst.setStatisticLoadLevel(7)
sst.setStatisticOutput("sst.statOutputCSV", {"filepath": str(trial / "network-statistics.csv")})
sst.enableAllStatisticsForAllComponents({"rate": "0ns"})
