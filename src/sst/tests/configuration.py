"""One disabled tile for parameter/validation checks; no guest execution."""
import json
import os
import sst

with open(os.environ['MITTENS_CONFIGURATION_INPUT']) as source:
    parameters = json.load(source)
resource = os.environ.get('MITTENS_CONFIGURATION_RESOURCE', 'mittens.tile')
if resource == 'mittens.wormholeNIC':
    # A real but idle endpoint exercises successful NIC/router setup without
    # launching QEMU or introducing a synthetic timing model.
    probe = sst.Component('configuration_probe', 'mittens.networkTimingProbe')
    probe.addParams({'endpoint_id': 0, 'network_size': 1})
    nic = probe.setSubComponent('networkIF', resource)
    nic.addParams({'endpoint_id': 0, 'network_size': 1,
                   'mesh_width': 1, 'mesh_height': 1, **parameters})
    lanes = parameters.get('tx_streams', 1)
    rx_lanes = parameters.get('rx_streams', 1)
    router = sst.Component('configuration_router', 'mittens.wormholeRouter')
    router.addParams({'id': 0, 'mesh_width': 1, 'mesh_height': 1,
                      'tx_streams': lanes,
                      'rx_streams': rx_lanes,
                      'link_width_bits': parameters.get('link_width_bits', 32)})
    for lane in range(max(lanes, rx_lanes)):
        link = sst.Link(f'configuration_local_{lane}')
        link.connect((nic, f'router_port{lane}', '1ns'),
                     (router, f'port{4 + lane}', '1ns'))
else:
    tile = sst.Component('configuration_test', resource)
    tile.addParams(parameters)
    if resource == 'mittens.globalRAMController' and os.environ.get('MITTENS_CONFIGURATION_POSITIVE') == '1':
        for index in range(parameters['tile_count']):
            probe = sst.Component(f'configuration_ram_probe_{index}', 'mittens.globalRAMReadinessProbe')
            probe.addParams({'tile_id': index, 'scenario': 'coverage'})
            link = sst.Link(f'configuration_ram_{index}')
            link.connect((probe, 'ram', '1ns'), (tile, f'dma{index}', '1ns'))
