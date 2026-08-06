#!/usr/bin/env python3

import json
import subprocess
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
EXPORTER = PROJECT_ROOT / "visualizer" / "exporter" / "export-profile.py"
PROFILE = Path(__file__).resolve().parent / "fixtures" / "profile"


def main():
    with tempfile.TemporaryDirectory(prefix="mittens-visualizer-") as directory:
        output = Path(directory) / "trace.json"
        subprocess.run(
            [
                str(EXPORTER),
                str(PROFILE),
                str(output),
                "--title",
                "Exporter test",
                "--source",
                "fixture",
            ],
            check=True,
        )
        trace = json.loads(output.read_text(encoding="utf-8"))

    assert trace["schemaVersion"] == 2
    assert trace["meta"]["width"] == 8
    assert trace["meta"]["height"] == 8
    assert trace["meta"]["durationTicks"] == 990000000
    assert trace["summary"] == {
        "tasks": 3,
        "routes": 2,
        "packets": 4,
        "dma": 2,
        "analog": 4,
        "waits": 2,
        "blocked": 0,
        "criticalTasks": 0,
        "contendedRoutes": 1,
        "injectedWords": 650,
        "activeTiles": 4,
    }
    assert trace["routes"][0]["path"][0] == 0
    assert trace["routes"][0]["path"][-1] == 63
    assert trace["routes"][0]["hops"] == 14
    assert trace["routes"][0]["contended"] is True
    assert trace["routes"][1]["contended"] is False
    assert trace["packets"][0]["kind"] == "frame-header"
    assert trace["packets"][1]["payloadWords"] == 512
    assert len(trace["links"]) == 16
    assert trace["tasks"][0]["start"] == 40000000
    assert trace["analog"][0]["start"] == 0
    print("Mittens visualization exporter: PASS")


if __name__ == "__main__":
    main()
