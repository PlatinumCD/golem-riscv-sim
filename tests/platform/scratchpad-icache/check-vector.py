#!/usr/bin/env python3
"""Check actual SPM transaction widths; functional success is insufficient."""
import csv
from pathlib import Path
import subprocess
import sys

profile, elf, nm = map(Path, sys.argv[1:])
symbols = {}
for line in subprocess.check_output([str(nm), str(elf)], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
with (profile/'tile-0-scratchpad-beats.csv').open() as stream:
    beats = list(csv.DictReader(stream))
for symbol, direction in [('input_values', 'read'), ('output_values', 'write')]:
    offset = symbols[symbol] - 0x90000000
    accesses = [b for b in beats if int(b['client']) == -1 and
                b['direction'] == direction and
                offset <= int(b['offset']) < offset + 32]
    wide = [b for b in accesses if int(b['bytes']) == 32]
    assert len(wide) == 1 and int(wide[0]['offset']) == offset, (symbol, accesses)
    # BSS initialization writes precede the vector store. No element stores
    # may follow it, and input has exactly one vector read and no scalar reads.
    if direction == 'read':
        assert len(accesses) == 1, accesses
    else:
        assert all(int(b['service_cycle']) < int(wide[0]['service_cycle'])
                   for b in accesses if b is not wide[0]), accesses
assert any(int(b['client']) == 9 for b in beats), 'instruction fills missing'
print('PASS: RVV load/store each use one 32-byte CPU beat with I-cache active')
