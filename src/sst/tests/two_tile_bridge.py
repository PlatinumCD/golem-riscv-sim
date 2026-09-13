import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/support"))
from mesh import build_mesh

build_mesh(width=2, height=1, qemu_path=os.environ["MITTENS_TEST_QEMU"],
           images=[os.environ["MITTENS_TILE0_ELF"], os.environ["MITTENS_TILE1_ELF"]],
           statistics_path=os.environ.get("MITTENS_PAIR_STATS", "pair-statistics.csv"),
           verbosity=2)
