"""Protocol check: an early tile reaches epoch one before a late tile boots."""
import sst
barrier=sst.Component('barrier','mittens.epochBarrierController')
barrier.addParams(dict(tile_count=2,active_tiles=[0,1],epoch_count=2,
    local_boot_release=True,release_cycles=1))
for tile,delay in ((0,1),(1,50)):
    probe=sst.Component(f'probe{tile}','mittens.epochBarrierProbe')
    probe.addParams(dict(tile_id=tile,epoch_count=2,arrival_delays=[delay,1],idle_epochs=[]))
    sst.Link(f'link{tile}').connect((probe,'barrier','1ns'),(barrier,f'barrier{tile}','1ns'))
