"""Two explicit router/SPM interfaces with finite posted-write reservations."""
import json
import os
from pathlib import Path
import sys
import sst

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import connect_scratchpad
from components.mordred.configuration import connect_mesh

trial = Path(os.environ['TILE_COMPONENT_OUTPUT'])
case = json.loads((trial/'case.json').read_text())
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '100us')
driver = sst.Component('driver', 'tilecomponents.MordredPostedTest')
driver.addParams(dict(sparse=case['sparse'], posted_enabled=case['slots'] > 0,
                      arrival_connected=case['arrivals']))
endpoints = []
for tile in range(2):
    prefix = f'posted.tile{tile}.'
    banks = 4 if case['sparse'] else 1
    allowed = [2, 3] if case['sparse'] and tile == 1 else list(range(banks))
    endpoint = sst.Component(prefix+'router_spm', 'tilecomponents.MordredSpmEndpoint')
    endpoint.addParams(dict(tile_id=tile, tile_count=2, spm_capacity_bytes=4096,
        spm_banks=banks, spm_bank_width=4, router_spm_banks=allowed,
        spm_request_bytes=32, request_window=4, max_request_bytes=256, memory_queue_depth=1,
        posted_receive_slots_per_source=case['slots'], posted_credit_batch=case['credit_batch'],
        posted_credit_delay_cycles=case['credit_delay'], flit_size_bits=case['link_bits']))
    memory = endpoint.setSubComponent('memory', 'memHierarchy.standardInterface')
    scratch = connect_scratchpad(sst, dict(spm_capacity_bytes=4096, spm_banks=banks,
        spm_bank_width=4, router_spm_banks=allowed, spm_request_bytes=32,
        spm_channels=1, spm_channel_width=4), [memory], name_prefix=prefix,
        router_requestor=prefix+'router_spm:memory')
    backing = trial/f'tile{tile}-spm.bin'
    backing.write_bytes(b'\x11'*4096)
    scratch.addParams(dict(backing='mmap', memory_file=str(backing)))
    sst.Link(prefix+'requests').connect((driver, f'requests{tile}', '1ns'), (endpoint, 'requests', '1ns'))
    if tile == 0 or case['arrivals']:
        sst.Link(prefix+'arrivals').connect((endpoint, 'arrivals', '1ns'), (driver, f'arrivals{tile}', '1ns'))
    endpoints.append(endpoint)
connect_mesh(sst, dict(x_dim=2, y_dim=1, flit_size_bits=case['link_bits'],
    router_input_buffer_flits=4, router_output_buffer_flits=4,
    nic_input_buffer_bytes=1024, nic_output_buffer_bytes=1024), name='fabric', endpoints=endpoints)
sst.setStatisticLoadLevel(7)
sst.setStatisticOutput('sst.statOutputCSV', {'filepath': str(trial/'network-statistics.csv')})
sst.enableAllStatisticsForAllComponents({'rate': '0ns'})
