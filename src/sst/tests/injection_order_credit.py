import sst
sst.setProgramOption('timebase', '1ps')
probe = sst.Component('probe', 'injection_credit_test.probe')
nic = probe.setSubComponent('nic', 'mittens.wormholeNIC')
nic.addParams({'endpoint_id':0,'network_size':2,'clock':'1GHz',
               'tx_streams':2,'link_width_bits':32})
for lane in range(2):
    sst.Link(f'lane{lane}').connect((probe,f'lane{lane}','1ns'),
                                   (nic,f'router_port{lane}','1ns'))
