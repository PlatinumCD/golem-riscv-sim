"""Drive the real array backend with a protocol-checking SST component."""
import json
import os
from pathlib import Path

import sst

trial = Path(os.environ['TILE_COMPONENT_OUTPUT'])
case = json.loads((trial/'case.json').read_text())
driver_name = case['driver']
capture = driver_name == 'CaptureProbe'
sst.setProgramOption('timebase', '1ps')
sst.setProgramOption('stop-at', '1ms')
driver = sst.Component('driver', 'tilecomponents.'+driver_name)
driver.addParams(dict(execute_cycles=100, deferred=case['deferred']))
arrays = sst.Component('arrays', 'tilecomponents.AnalogArrays')
arrays.addParams(dict(array_rows=8 if capture else 2, array_cols=8 if capture else 2,
    arrays_per_tile=2 if capture else 1, riscv_vector_length_bits=128,
    array_inflight_bytes=32, array_link_duplex='shared',
    array_pipeline_enabled=driver_name != 'NonpipelineProbe',
    array_program_delay_scope='initial_full_array' if driver_name == 'EpochProbe' else 'per_command',
    cost_per_array_program_cycles=7, cost_per_mvm_cycles=100))
sst.Link('commands').connect((driver, 'commands', '1ns'), (arrays, 'commands', '1ns'))
