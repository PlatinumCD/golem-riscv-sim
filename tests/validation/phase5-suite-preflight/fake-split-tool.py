#!/usr/bin/env python3
"""Test double for the Phase-5 suite preflight wrapper."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys


artifact = Path(sys.argv[1])
log = os.environ.get("PHASE5_PREFLIGHT_FAKE_LOG")
if log:
    with Path(log).open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(sys.argv[1:]) + "\n")

mode = artifact.read_text(encoding="utf-8").strip()
if mode == "compiler-error":
    print("selected scalar input M1 descriptor family has stale boundary authority", file=sys.stderr)
    sys.exit(1)

version = 2 if mode == "stale-schema" else 3
selected = 4 if mode == "incoherent-total" else 3
print(f"scalar_plan_version={version}")
print("scalar_eligible_region_count=2")
print("scalar_fallback_region_count=1")
print(f"scalar_selected_region_count={selected}")
print("scalar_removed_kernel_count=3")
