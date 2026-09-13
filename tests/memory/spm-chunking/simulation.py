"""One executable-SPM tile, no guest RAM, and explicitly timed shared RAM DMA."""
import os
import sst

profile = os.environ['MITTENS_TEST_PROFILE']
tile = sst.Component('tile0', 'mittens.tile')
tile.addParams(dict(
    tile_id=0, qemu_path=os.environ['MITTENS_TEST_QEMU'], elf=os.environ['MITTENS_TEST_ELF'],
    memory='16M', launch_mode='managed', cpu_clock='1GHz', cpu_issue_width=1,
    sync_instruction_quantum=100, memory_backend='streaming', scratchpad_boot=True,
    scratchpad_enabled=True, scratchpad_bytes=int(os.environ.get('MITTENS_TEST_SPM_BYTES','16384')), scratchpad_banks=8,
    scratchpad_access_width_bits=256, scratchpad_read_ports=1, scratchpad_write_ports=1,
    scratchpad_latency_cycles=1, scratchpad_dma_bytes_per_cycle=32, scratchpad_dma_setup_cycles=8,
    instruction_cache_bytes=8192, instruction_cache_line_bytes=64, instruction_cache_ways=2,
    memory_init_batching=False, memory_access_batching=False, scratchpad_access_batching=False,
    scratchpad_access_run_compaction=False, memory_event_batching=False,
    global_dma_submit_batching=False, global_dma_macro_execution=False,
    qemu_ready_set_workers=1, qemu_runtime_ready_set=False, qemu_local_lookahead=False,
    global_ram_bytes=4*1024*1024, profile_mode='trace', profile_output_directory=profile,
    task_trace_directory=profile+'/tasks', verbose=1))
ram = sst.Component('global_ram', 'mittens.globalRAMController')
ram.addParams(dict(tile_count=1, active_tiles=[0], capacity_bytes=4*1024*1024,
    dependency_mode='bulk_barrier', clock='1GHz', channels=1, queue_depth=8,
    per_tile_queue_depth=8, setup_cycles=8, bytes_per_cycle=32, burst_bytes=64,
    fixed_latency_cycles=2, profile_output_directory=profile))
link = sst.Link('tile0.global_dma')
link.connect((tile, 'globalDMA', '1ns'), (ram, 'dma0', '1ns'))
link.setNoCut()
