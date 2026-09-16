import os
from pathlib import Path
import sys
import sst
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/support'))
from mesh import build_mesh
out = Path(os.environ['DMA_QUERY_CASE'])
install = Path(os.environ['GOLEM_INSTALL_ROOT'])
mode = int(os.environ['DMA_QUERY_MODE'])
guest = Path(os.environ.get('DMA_QUERY_GUEST', str(out.parent/'guest')))
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '10ms')
routers = build_mesh(width=2, height=1,
    statistics_path=str(out/'statistics.csv'),
    qemu_path=str(install/'qemu/bin/qemu-system-riscv64'),
    images=[str(guest/f'm{mode}-t{i}.elf') for i in range(2)],
    active_tiles=[0,1], memory_backend='streaming', verbosity=0,
    global_memory=dict(image_file=str(out.parent/'ram.bin'), image_offset=0,
                       channels=1, bytes_per_cycle=4, burst_bytes=64,
                       queue_depth=16, per_tile_queue_depth=8),
    tile_params=dict(scratchpad_bytes=262144, global_ram_bytes=1048576,
                     sync_instruction_quantum=int(os.environ.get('DMA_QUERY_QUANTUM', '1000')),
                     global_dma_submit_batching=False, global_dma_macro_execution=False,
                     task_trace_directory=str(out/'tasks'),
                     serial_output_directory=str(out/'serial'),
                     profile_mode='trace', profile_output_directory=str(out/'profile')))
if os.environ.get('DMA_QUERY_ROUTER_TRACE') == '1':
    for (x, y), router in routers.items():
        router.addParam('packet_trace_path', str(out/'profile'/f'router-{x + 2*y}-packets.csv'))
