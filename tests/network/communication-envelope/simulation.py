"""One frozen machine; only case-declared workload/configuration controls vary."""
import json
import os
import sys
from pathlib import Path
import sst

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/support'))
from mesh import build_mesh

trial = Path(os.environ['ENVELOPE_TRIAL'])
case = json.loads((trial / 'case.json').read_text())
images = [''] * (case['width'] * case['height'])
for tile in case['active_tiles']:
    images[tile] = str(trial / 'elf' / f'tile{tile}.elf')
sst.setProgramOption('timebase', '1ps')
build_mesh(width=case['width'], height=case['height'],
    qemu_path=os.environ['MITTENS_TEST_QEMU'], images=images, active_tiles=case['active_tiles'],
    statistics_path=str(trial / 'router-statistics.csv'), verbosity=0,
    tile_params={
        'memory': '16M', 'cpu_clock': '1GHz', 'cpu_issue_width': 1,
        'sync_instruction_quantum': 1000,
        'scratchpad_enabled': True, 'scratchpad_bytes': 524288, 'scratchpad_banks': 8,
        'scratchpad_read_ports': 1, 'scratchpad_write_ports': 1,
        'scratchpad_access_width_bits': 256, 'scratchpad_latency_cycles': 1,
        'scratchpad_access_batching': True, 'scratchpad_access_run_compaction': True,
        'riscv_vector_enabled': True, 'riscv_vector_length_bits': 256,
        'riscv_vector_element_bits': 32, 'rx_dma_width_bits': 256,
        'rx_dma_setup_cycles': 8, 'rx_dma_queue_depth': case['rx_queue'],
        'tx_dma_streams': case['tx_lanes'], 'tx_dma_fifo_bytes': case['fifo'],
        'memory_init_batching': True, 'memory_init_instruction_quantum': 1000,
        'memory_init_bytes_per_cycle': 32, 'memory_init_latency_cycles': 2,
        'profile_mode': os.environ.get('ENVELOPE_PROFILE_MODE', 'trace'),
        'profile_output_directory': str(trial / 'profile'),
        'task_trace_directory': str(trial / 'tasks'),
        'serial_output_directory': str(trial / 'serial'),
    }, memory_backend='streaming',
    initialization_barrier={'clock':'1GHz', 'release_cycles':1,
                            'release_tiles_per_cycle':len(case['active_tiles'])},
    mesh_router_backend='mittens',
    network_cell_words=1, network_buffer_cells=16384, network_packet_words=64,
    mesh_link_width_bits=32, mesh_link_clock='1GHz',
    wormhole_input_buffer_flits=128, wormhole_injection_buffer_flits=128)
