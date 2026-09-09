#!/usr/bin/env python3
"""Print the tile parameter reference, or check the checked-in copy."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REFERENCE = ROOT / 'docs/parameters.md'


def render():
    # Expand the production macro with the C++ preprocessor. This preserves
    # expression defaults and adjacent string literals without parsing C++.
    source = r'''
#include <cstdint>
#include <iostream>
#include <string>
#include "src/sst/configuration/tileParameters.h"
int main() {
    std::cout << std::boolalpha;
#define ROW(type, field, key, value, category, help, documented) \
    std::cout << "| `" << key << "` | `" << #type << "` | `" \
              << type(value) << "` | " << category << " | " << help << " |\n";
    MITTENS_TILE_PARAMETERS(ROW)
#undef ROW
}
'''
    with tempfile.TemporaryDirectory(prefix='golem-parameter-reference-') as temp:
        executable = Path(temp) / 'reference'
        subprocess.run(['c++', '-std=c++17', '-I', str(ROOT), '-x', 'c++', '-',
                        '-o', str(executable)], input=source, text=True, check=True)
        rows = subprocess.check_output([str(executable)], text=True).splitlines()
    header = (ROOT / 'src/sst/configuration/tileParameters.h').read_text()
    groups = re.findall(r'// ([^\n]+)\n#define MITTENS_TILE_\w+_PARAMETERS\(X\)', header)
    counts = [len(re.findall(r'\bX\(', block)) for block in re.split(
        r'#define MITTENS_TILE_\w+_PARAMETERS\(X\)', header)[1:]]
    # The final public macro composes groups; it contains no entries.
    counts = [count for count in counts if count]
    if len(groups) != len(counts) or sum(counts) != len(rows):
        raise RuntimeError('parameter groups and macro expansion disagree')
    lines = ['# Tile parameters', '',
             'Generated from [tileParameters.h](../src/sst/configuration/tileParameters.h).',
             'Values below are defaults; a simulation may override them.', '',
             'Set these keys on `mittens.tile` in the SST configuration. Invalid combinations',
             'are rejected by `TileConfiguration::validate`.', '',
             'Categories: **hardware** describes modeled resources, **execution** controls',
             'host execution and replay, **workload** selects guest inputs, and',
             '**measurement** controls output. `scratchpad_access_width_bits` is currently',
             'exported as execution metadata but sets the modeled SPM port width.', '',
             'Regenerate with `python3 tools/hardware/parameter-reference.py`;',
             'use `--check` to detect stale documentation.', '']
    offset = 0
    for group, count in zip(groups, counts):
        lines += ['## ' + group.rstrip('.'), '',
                  '| Parameter | Type | Default | Category | Meaning |',
                  '|---|---|---|---|---|'] + rows[offset:offset + count] + ['']
        offset += count
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    document = render()
    if args.check:
        if not REFERENCE.exists() or REFERENCE.read_text() != document:
            parser.exit(1, 'docs/parameters.md is stale; regenerate the parameter reference\n')
        print('Parameter reference: current')
    else:
        print(document, end='')


if __name__ == '__main__':
    main()
