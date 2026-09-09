#!/usr/bin/env python3
"""Fixed TX correctness fixtures, reusing the existing fan-out guest (no study sweep)."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/hardware'))
from hardware_paths import require_owned_output, resolve_paths


def main():
    paths = resolve_paths()
    build = Path(paths['GOLEM_BUILD_ROOT'])
    install = Path(paths['GOLEM_INSTALL_ROOT'])
    output = build / 'tx-controller'
    require_owned_output(output, build)
    output.mkdir(parents=True, exist_ok=False)
    kernel = ROOT / 'studies/banks-channels/tx-fanout-opportunity'
    # Each entry is (pattern, bank layout, FIFO bytes, lanes, active tiles).
    # Layout 2 pads independent buffers; keep the same 32 KiB SPM in every case.
    fixtures = [
        (0, 0, 128, lanes, (7, 11, 12, 13, 17)) for lanes in (1, 2, 4)
    ] + [
        (1, 0, 128, lanes, (10, 11, 12, 13, 14)) for lanes in (1, 4)
    ] + [
        (2, 0, 128, 2, (2, 7, 12, 13, 14)),
        (0, 2, 128, 4, (7, 11, 12, 13, 17)),
        (0, 0, 32, 4, (7, 11, 12, 13, 17)),
    ]
    for pattern, banks, fifo, lanes, active in fixtures:
        trial = output / f'p{pattern}-banks{banks}-fifo{fifo}-t{lanes}'
        for folder in ('profile', 'tasks', 'serial'):
            (trial / folder).mkdir(parents=True)
        env = dict(os.environ, **paths, PYTHONDONTWRITEBYTECODE='1')
        with (trial / 'build.log').open('w') as log:
            subprocess.run(['make', '-C', str(kernel), f'OUTPUT_DIR={trial / "elf"}',
                            f'PATTERN={pattern}', f'BANK_LAYOUT={banks}',
                            'TILES=' + ' '.join(map(str, active))],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        env.update(
            SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'),
            MITTENS_TEST_QEMU=str(install / 'qemu/bin/qemu-system-riscv64'),
            MITTENS_TX_FANOUT_ELF_DIR=str(trial / 'elf'),
            MITTENS_TX_FANOUT_PROFILE=str(trial / 'profile'),
            MITTENS_TX_FANOUT_TASKS=str(trial / 'tasks'),
            MITTENS_TX_FANOUT_SERIAL=str(trial / 'serial'),
            MITTENS_TX_FANOUT_STATS=str(trial / 'router-statistics.csv'),
            MITTENS_TX_STREAMS=str(lanes), MITTENS_FANOUT_PATTERN=str(pattern),
            MITTENS_TX_DMA_FIFO_BYTES=str(fifo), MITTENS_TX_FANOUT_SPM_BYTES='32768')
        with (trial / 'simulation.out').open('w') as log:
            subprocess.run([str(install / 'sst-core/bin/sst'), str(kernel / 'simulation.py')],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        serial = ''.join(path.read_text() for path in (trial / 'serial').glob('*') if path.is_file())
        if (serial.count('TX_FANOUT_SOURCE_PASS') != 1 or
                serial.count('TX_FANOUT_SINK_PASS') != len(active) - 1 or 'FAIL' in serial):
            raise RuntimeError(f'incomplete or incorrect guest result: {trial}')
        print(f'TX fixture {trial.name}: PASS', flush=True)


if __name__ == '__main__':
    main()
