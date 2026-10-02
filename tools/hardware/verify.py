#!/usr/bin/env python3
"""Run current-model configuration and build/runner contract checks without SST."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    for relative in ('src/tests/test_configuration.py', 'src/tests/mordred/test_configuration.py',
                     'src/tests/network-instructions/test_configuration.py',
                     'src/tests/dram-tile/test_configuration.py',
                     'src/tests/sst-threads/test_configuration.py', 'tools/hardware/tests/test-current-model.py'):
        subprocess.run([sys.executable, '-B', str(ROOT/relative)], check=True)
    subprocess.run([sys.executable, '-B', str(ROOT/'tools/hardware/parameter-reference.py'), '--check'], check=True)
    print('PASS current-model host checks')


if __name__ == '__main__': main()
