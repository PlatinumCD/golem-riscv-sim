"""Assertions on a measured guest interval, excluding boot and setup."""
import csv
from pathlib import Path


def assert_register_only(profile, start, finish):
    """Allow instruction-cache fills, reject data accesses and DMA in the ROI."""
    profile = Path(profile)
    begin, end = int(start['sim_time_ticks']), int(finish['sim_time_ticks'])
    assert end > begin
    # These fixtures run at 1 GHz with SST's 1 ps timebase.
    with (profile / 'tile-0-scratchpad-beats.csv').open() as stream:
        beats = list(csv.DictReader(stream))
    assert beats, 'missing SPM evidence (boot must have written the image)'
    active = [b for b in beats if begin <= int(b['issue_cycle']) * 1000 < end]
    unexpected = [b for b in active if int(b['client']) != 9]
    assert not unexpected, f'non-instruction SPM access inside register-only region: {unexpected[:5]}'
    return sum(int(b['bytes']) for b in active)
