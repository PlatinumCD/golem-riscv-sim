"""Four deterministic endpoints on a 2x2 Mordred mesh; run through run.py."""
import json
import os
from pathlib import Path
import sys

import sst

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from components.mordred.configuration import MeshParameters, connect_mesh

case_path = os.environ.get("MORDRED_TEST_CASE")
case = json.loads(Path(case_path).read_text()) if case_path else {}
parameters = MeshParameters(**case.get("mesh", {}))
output = Path(case_path).parent if case_path else None
endpoints = []
for endpoint_id in range(parameters.endpoint_count):
    endpoint = sst.Component(f"endpoint{endpoint_id}", "mordredtests.meshEndpoint")
    endpoint.addParams(dict(id=endpoint_id, num_peers=parameters.endpoint_count,
        num_vns=parameters.num_vns, flit_size_bits=parameters.flit_size_bits,
        clock=parameters.clock, **case.get("endpoint", {})))
    if output:
        endpoint.addParam("result_file", str(output / f"endpoint{endpoint_id}.json"))
    endpoints.append(endpoint)

mesh = connect_mesh(sst, parameters, endpoints=endpoints)
sst.setStatisticLoadLevel(7)
if output:
    sst.setStatisticOutput("sst.statOutputCSV", {"filepath": str(output / "statistics.csv")})
    sst.enableAllStatisticsForAllComponents({"rate": "0ns"})
