#!/usr/bin/env python3
"""Generate the current composition defaults without loading SST."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src'))
from configuration import DEFAULTS, CPU_DEFAULTS, resolve
from components.mordred.configuration import MeshParameters
from components.mordred.tiles import ROUTER_DEFAULTS
REFERENCE = ROOT / 'docs/parameters.md'


def render():
    lines = ['# Simulation parameters', '',
        'Generated from [tile/CPU composition](../src/configuration.py),',
        '[mesh settings](../src/components/mordred/configuration.py), and',
        '[NIU settings](../src/components/mordred/tiles.py).', '',
        'These are standalone defaults. The complete-tile mesh helper defaults to four',
        'SPM banks with all banks accessible to the CPU and the highest two to the router.',
        'Bank lists select physical addresses and shared resources; they are not bandwidth quotas.', '',
        'Parameter meanings and constraints: [tile and array parameters](../src/README.md#architecture-parameters),',
        '[CPU](../src/components/riscv-qemu/README.md), [mesh](../src/components/mordred/README.md),',
        '[router-facing SPM service](../src/components/mordred/spm-interface.md).', '',
        'Regenerate with `python3 tools/hardware/parameter-reference.py > docs/parameters.md`.', '']
    for title, values in [('Tile, scratchpad and accelerator', DEFAULTS), ('CPU', CPU_DEFAULTS),
                           ('Router-facing SPM interface', ROUTER_DEFAULTS), ('Mesh and NIC', asdict(MeshParameters()))]:
        lines += ['## '+title, '', '| Parameter | Default |', '|---|---|']
        lines += [f'| `{name}` | `{json.dumps(value)}` |' for name, value in values.items()]
        lines += ['']
    lines += ['`array_link_width` is derived as `riscv_vector_length_bits / 8`.',
              f'The standalone default resolves to {resolve()["array_link_width"]} bytes/cycle.', '']
    return '\n'.join(lines)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args=parser.parse_args(); text=render()
    if args.check:
        if not REFERENCE.is_file() or REFERENCE.read_text()!=text:
            parser.exit(1, 'docs/parameters.md is stale; regenerate it\n')
        print('Parameter reference: current')
    else: print(text, end='')


if __name__ == '__main__': main()
