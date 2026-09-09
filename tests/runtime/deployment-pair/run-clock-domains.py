#!/usr/bin/env python3
"""Assert real ordinary-RAM RX timings with different CPU and DMA clocks."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def main():
    variant = 'src' if os.environ.get('GOLEM_HARDWARE_TREE') == 'src' else ''
    build = Path(os.environ.get('GOLEM_BUILD_ROOT', ROOT / 'build' / variant))
    install = Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install' / variant))
    kernel = build / 'tests/deployment-runtime-pair'
    output = kernel / 'mixed-clocks'
    output.mkdir(parents=True, exist_ok=True)
    # The ordinary one-clock gate builds and checks the exact same payload first.
    with (output / 'baseline.log').open('w') as log:
        subprocess.run(['bash', str(HERE / 'run-test.sh')], check=True,
                       stdout=log, stderr=subprocess.STDOUT, timeout=120)
    cases = [('1GHz', '500MHz', 92), ('2GHz', '1GHz', 92), ('500MHz', '1GHz', 23)]
    results = []
    for cpu, dma, expected in cases:
        for spm in ('false', 'true'):
            trial = output / f'cpu-{cpu}-rx-{dma}-spm-{spm}'
            serial = trial / 'serial'
            serial.mkdir(parents=True, exist_ok=True)
            env = dict(os.environ,
                       SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'),
                       MITTENS_TEST_QEMU=str(install / 'qemu/bin/qemu-system-riscv64'),
                       MITTENS_DEPLOYMENT_TILE0_ELF=str(kernel / 'tile0.elf'),
                       MITTENS_DEPLOYMENT_TILE1_ELF=str(kernel / 'tile1.elf'),
                       MITTENS_DEPLOYMENT_STATS=str(trial / 'router.csv'),
                       MITTENS_DEPLOYMENT_SERIAL=str(serial),
                       MITTENS_DEPLOYMENT_CPU_CLOCK=cpu,
                       MITTENS_DEPLOYMENT_RX_DMA_CLOCK=dma,
                       MITTENS_DEPLOYMENT_SCRATCHPAD_ENABLED=spm)
            with (trial / 'simulation.log').open('w') as log:
                subprocess.run([str(install / 'sst-core/bin/sst'), str(HERE / 'simulation.py')],
                               env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
            assert 'DEPLOYMENT_RUNTIME_TILE_0_PASS' in (serial / 'tile-0.log').read_text()
            assert 'DEPLOYMENT_RUNTIME_TILE_1_PASS words=300' in (serial / 'tile-1.log').read_text()
            text = (trial / 'simulation.log').read_text()
            profiles = re.findall(r'MITTENS_PROFILE tile=1 .*', text)
            assert len(profiles) == 1, (trial, profiles)
            cycles = int(re.search(r'rx_dma_active_cycles=(\d+)', profiles[0])[1])
            words = int(re.search(r'rx_dma_words=(\d+)', profiles[0])[1])
            assert (cycles, words) == (expected, 300), (cpu, dma, spm, cycles, words)
            results.append({'cpu_clock': cpu, 'rx_clock': dma, 'spm_enabled': spm == 'true',
                            'rx_service_cpu_cycles': cycles, 'words': words, 'status': 'PASS'})
            print(f'RX clocks CPU={cpu} DMA={dma} SPM={spm}: {cycles} CPU cycles PASS', flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
