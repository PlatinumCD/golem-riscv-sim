import os
from pathlib import Path
import sys
import sst

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tests/support'))
from mesh import build_mesh

trial = Path(os.environ['ACCOUNTING_TRIAL'])
quantum = int(os.environ['ACCOUNTING_QUANTUM'])
sst.setProgramOption('timebase', '1ps')
build_mesh(width=1,height=1,qemu_path=os.environ['ACCOUNTING_QEMU'],
    images=[os.environ['ACCOUNTING_ELF']],active_tiles=[0],
    statistics_path=str(trial/'router.csv'),verbosity=0,memory_backend='streaming',
    tile_params={
        'cpu_clock':'1GHz','cpu_issue_width':1,'sync_instruction_quantum':quantum,

        'scratchpad_enabled':True,'scratchpad_bytes':262144,
        'scratchpad_banks':8,'scratchpad_read_ports':1,'scratchpad_write_ports':1,
        'scratchpad_access_width_bits':256,'scratchpad_latency_cycles':1,

        'riscv_vector_enabled':True,'riscv_vector_length_bits':256,'riscv_vector_element_bits':32,
        'profile_mode':'trace','profile_output_directory':str(trial/'profile'),
        'task_trace_directory':str(trial/'tasks'),'serial_output_directory':str(trial/'serial')},
    mesh_router_backend='mittens',mesh_link_width_bits=32,mesh_link_clock='1GHz')
