#!/usr/bin/env python3
"""Compare serial and concurrent host capture without changing guest timing."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
INSTALL = Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install/src'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--element-library', type=Path,
                        default=INSTALL / 'sst-elements/lib/sst-elements-library')
    parser.add_argument('--fetch-segment-size', type=int, choices=range(1, 17), default=1)
    args = parser.parse_args()
    out = Path(os.environ.get('GOLEM_TEST_RESULTS_ROOT', ROOT / 'tests/results')) / 'concurrent-capture' / str(time.time_ns())
    guests = out / 'guest'
    subprocess.run(['make', '-C', str(ROOT / 'tests/platform/scratchpad-icache'),
                    f'OUT={guests}', 'all'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['bash', str(ROOT / 'build-scripts/build-platform.sh'), 'mesh-pair'],
                   env=dict(os.environ, GOLEM_TEST_RESULTS_ROOT=str(out)),
                   check=True, stdout=subprocess.DEVNULL)
    print(out, flush=True)
    results = []
    for shape, programs in [('aligned', 'rvv-spm,rvv-spm,rvv-spm,rvv-spm'),
                            ('mixed', 'warm-loop,rvv-spm,fence-i,rvv-fault'),
                            ('network', 'tile0-sender,tile1-receiver')]:
        guest_dir = out / 'mesh-pair' if shape == 'network' else guests
        baseline = None
        for workers in (1, 2, 4):
            case = out / f'{shape}-w{workers}'
            case.mkdir(parents=True)
            env = dict(os.environ, SST_LIB_PATH=str(args.element_library.resolve()),
                       MITTENS_TEST_QEMU=str(INSTALL / 'qemu/bin/qemu-system-riscv64'),
                       CAPTURE_OUTPUT=str(case), CAPTURE_GUESTS=str(guest_dir),
                       CAPTURE_FETCH_SEGMENT=str(args.fetch_segment_size),
                       CAPTURE_WORKERS=str(workers), CAPTURE_PROGRAMS=programs)
            started = time.monotonic()
            with (case / 'run.log').open('w') as log:
                proc = subprocess.Popen([str(INSTALL / 'sst-core/bin/sst'),
                                         str(HERE / 'concurrent_capture_simulation.py')],
                                        env=env, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
                try:
                    code = proc.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    raise RuntimeError(f'timed out: {case}')
            elapsed = time.monotonic() - started
            text = (case / 'run.log').read_text()
            assert code == 0 and 'Simulation is complete' in text, case
            serial = sorted((case / 'serial').glob('*.log'))
            assert len(serial) == len(programs.split(','))
            assert all('ERROR' not in p.read_text() for p in serial), case
            if shape == 'network':
                assert 'acknowledgment received; done' in serial[0].read_text(), case
                assert 'received float32 3.25' in serial[1].read_text(), case
            else:
                assert all('SCRATCHPAD_ICACHE_PASS' in p.read_text() for p in serial), case
            # Compare all deterministic per-tile traces and summaries. Progress
            # snapshots contain host timestamps and are deliberately excluded.
            evidence = {str(p.relative_to(case)): p.read_bytes()
                        for directory in ('profile', 'serial')
                        for p in (case / directory).glob('*')
                        if p.is_file() and 'progress' not in p.name}
            evidence['statistics.csv'] = (case / 'statistics.csv').read_bytes()
            for key, content in list(evidence.items()):
                if key.endswith('-summary.json'):
                    summary = json.loads(content)
                    # The referenced configuration path includes this run's PID.
                    summary['metadata'].pop('resolved_config_reference')
                    evidence[key] = json.dumps(summary, sort_keys=True).encode()
            if baseline is None:
                baseline = evidence
            else:
                differences = [key for key in baseline.keys() | evidence.keys()
                               if baseline.get(key) != evidence.get(key)]
                assert not differences, (case, differences)
                assert re.search(r'parallel_dispatches=[1-9]', text), case
            row = dict(shape=shape, workers=workers, host_seconds=elapsed,
                       status='validated', exact_trace_identity=True)
            results.append(row)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(row, flush=True)


if __name__ == '__main__':
    main()
