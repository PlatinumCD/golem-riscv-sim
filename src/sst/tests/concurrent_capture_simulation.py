"""Four executable-SPM guests sharing timed boot DMA; host workers only vary."""
import os
from pathlib import Path
import sys
import sst

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/support'))
from mesh import build_mesh

out = Path(os.environ['CAPTURE_OUTPUT'])
for directory in ('serial', 'profile'):
    (out / directory).mkdir(exist_ok=True)
guest = Path(os.environ['CAPTURE_GUESTS'])
names = os.environ.get('CAPTURE_PROGRAMS', 'rvv-spm,rvv-spm,rvv-spm,rvv-spm').split(',')
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '1ms')
build_mesh(width=2, height=len(names) // 2, images=[str(guest / (name + '.elf')) for name in names],
           qemu_path=os.environ['MITTENS_TEST_QEMU'], memory_backend='streaming',
           statistics_path=str(out / 'statistics.csv'), verbosity=0,
           global_memory=dict(channels=2, bytes_per_cycle=32, queue_depth=32,
                              per_tile_queue_depth=8, setup_cycles=8, burst_bytes=64,
                              fixed_latency_cycles=2),
           tile_params=dict(qemu_capture_workers=int(os.environ['CAPTURE_WORKERS']),
                            instruction_fetch_segment_size=int(os.environ.get('CAPTURE_FETCH_SEGMENT', '1')),
                            sync_instruction_quantum=7, profile_mode='trace',
                            profile_output_directory=str(out / 'profile'),
                            serial_output_directory=str(out / 'serial')))
