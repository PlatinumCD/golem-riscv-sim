#!/usr/bin/env python3
"""Validate fetch grouping against the ungrouped executable-SPM model."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
GUEST = ROOT / 'tests/platform/scratchpad-icache'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', type=Path,
                        default=Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install/src')))
    parser.add_argument('--register-iterations', type=int, default=100,
                        help='Use a larger register loop for host-time measurement')
    args = parser.parse_args()
    if args.register_iterations < 1:
        parser.error('--register-iterations must be positive')
    install = args.install.resolve()
    out = Path(os.environ.get('GOLEM_TEST_RESULTS_ROOT', ROOT / 'tests/results')) / 'fetch-segments' / str(time.time_ns())
    guests = out / 'guest'
    subprocess.run(['make', '-C', str(GUEST), f'OUT={guests}',
                    f'REGISTER_ITERATIONS={args.register_iterations}', 'all'], check=True,
                   stdout=subprocess.DEVNULL)
    print(out, flush=True)
    cases = [('register-segment', 1000, 1, (1, 4, 8, 16)),
             ('register-segment', 7, 1, (1, 16)),
             ('register-segment', 1000, 3, (1, 16))]
    cases += [(name, 7, 1, (1, 16)) for name in
              ('warm-loop', 'eviction', 'straddle', 'fence-i', 'trap-ram',
               'rvv-spm', 'rvv-fault', 'reload-code', 'dma-code', 'register-trigger')]
    results = []
    for name, quantum, hit, limits in cases:
        baseline = None
        for limit in limits:
            trial = out / f'{name}-q{quantum}-h{hit}-s{limit}'
            (trial / 'profile').mkdir(parents=True)
            env = dict(os.environ, SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'),
                       MITTENS_TEST_QEMU=str(install / 'qemu/bin/qemu-system-riscv64'),
                       MITTENS_TEST_ELF=str(guests / f'{name}.elf'),
                       MITTENS_TEST_PROFILE=str(trial / 'profile'),
                       MITTENS_TEST_QUANTUM=str(quantum), MITTENS_TEST_HIT_CYCLES=str(hit),
                       MITTENS_TEST_FETCH_SEGMENT_SIZE=str(limit))
            start = time.monotonic()
            with (trial / 'run.log').open('w') as log:
                proc = subprocess.Popen([str(install / 'sst-core/bin/sst'), str(GUEST / 'simulation.py')],
                                        env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = proc.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    raise RuntimeError(f'timeout: {trial}')
            elapsed = time.monotonic() - start
            text = (trial / 'run.log').read_text()
            assert code == 0 and 'SCRATCHPAD_ICACHE_PASS' in text and 'Simulation is complete' in text, trial
            cache = re.search(r'INSTRUCTION_CACHE tile=0 (.*)', text).group(1)
            with (trial / 'profile/tile-0-summary.csv').open() as f:
                stats = dict(list(csv.reader(f))[1:])
            handoffs = int(stats['stop_instruction_fetch'])
            # Only transport event counts and host-triggered snapshot counts
            # can differ. Retired work, timing, SPM and device counters cannot.
            for key in ('synchronization_events', 'stop_instruction_fetch',
                        'progress_snapshots', 'progress_counter_flushes'):
                stats.pop(key)
            evidence = (cache, stats)
            if baseline is None:
                baseline = evidence
                original_handoffs = handoffs
            else:
                assert evidence == baseline, (trial, {k:(baseline[1].get(k),stats.get(k))
                    for k in baseline[1].keys() | stats.keys() if baseline[1].get(k)!=stats.get(k)}, cache, baseline[0])
                if name == 'register-segment':
                    assert handoffs < original_handoffs, 'register segment did not reduce handoffs'
            row = dict(test=name, quantum=quantum, hit_cycles=hit, segment_size=limit,
                       register_iterations=args.register_iterations,
                       host_seconds=elapsed, fetch_handoffs=handoffs, status='validated')
            results.append(row)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(row, flush=True)


if __name__ == '__main__':
    main()
