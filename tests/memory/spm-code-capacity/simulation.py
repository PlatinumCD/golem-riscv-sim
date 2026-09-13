"""Reuse the chunking test's hardware, with an explicitly fixed 32 KiB SPM."""
import os
from pathlib import Path
import runpy
os.environ['MITTENS_TEST_SPM_BYTES']='32768'
runpy.run_path(str(Path(__file__).resolve().parent.parent/'spm-chunking/simulation.py'))
