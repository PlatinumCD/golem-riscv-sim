"""Check real array admission, result reservations and timed source capture."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
DRIVERS = {
    'PipelineProtocol': 'PIPELINE_PROTOCOL_RESULT',
    'EpochProbe': 'EPOCH_RESULT',
    'NonpipelineProbe': 'NONPIPELINE_RESULT',
    'CaptureProbe': 'CAPTURE_RESULT',
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def record(log, prefix):
    matches = [json.loads(line[len(prefix)+1:]) for line in log.splitlines()
               if line.startswith(prefix+' ')]
    assert len(matches) == 1, (prefix, matches)
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sst-core', type=Path, default=ROOT/'install/sst-core')
    parser.add_argument('--array-source', type=Path, default=SOURCE/'components/analog-arrays',
                        help='Directory containing the array backend and command header')
    args = parser.parse_args()
    output = (args.output or ROOT/'tests/results/source-new-analog-command-queue-backend'/str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    backend = args.array_source.resolve()
    core = args.sst_core.resolve()
    print(output, flush=True)
    inputs = [p for p in HERE.iterdir() if p.is_file()]
    inputs += [backend/name for name in ('commands.h', 'analogArrays.h', 'analogArrays.cc')]
    inputs += [backend.parent/name for name in ('fixed.h', 'observations.h')]
    before = {str(p): sha(p) for p in inputs}
    config = core/'bin/sst-config'
    compiler = shlex.split(subprocess.check_output([str(config), '--CXX'], text=True))
    flags = shlex.split(subprocess.check_output([str(config), '--ELEMENT_CXXFLAGS'], text=True))
    build = output/'build'
    build.mkdir()
    command = compiler+flags+['-O2', '-Wall', '-Wextra', '-shared', '-pthread',
        f'-I{backend}', str(backend/'analogArrays.cc'),
        str(HERE/'pipeline.cc'), str(HERE/'epoch.cc'), '-o', str(build/'libtilecomponents.so')]
    process = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (build/'build.log').write_text(process.stdout+process.stderr)
    process.check_returncode()
    build_info = dict(command=command, library_sha256=sha(build/'libtilecomponents.so'))
    (build/'build.json').write_text(json.dumps(build_info, indent=2)+'\n')
    results = []
    for enabled in (False, True):
        for driver, prefix in DRIVERS.items():
            trial = output/f'deferred-{int(enabled)}-{driver}'
            trial.mkdir()
            case = dict(driver=driver, deferred=enabled)
            (trial/'case.json').write_text(json.dumps(case, indent=2)+'\n')
            environment = os.environ | dict(SST_LIB_PATH=str(build),
                TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1')
            environment.pop('TILE_COMPONENT_TRACE_START_TASK', None)
            environment.pop('TILE_COMPONENT_PROGRAM_PROOF', None)
            process = subprocess.run([str(core/'bin/sst'), '--num-threads=1',
                f"--output-json={trial/'topology.json'}", str(HERE/'simulation.py')],
                env=environment, capture_output=True, text=True, timeout=30)
            log = process.stdout+process.stderr
            (trial/'simulation.log').write_text(log)
            process.check_returncode()
            proof = record(log, prefix)
            assert proof['passed'] is True, proof
            result = case | dict(passed=True, proof=proof, array_stats=record(log, 'ARRAY_STATS'),
                                  traces={p.name: sha(p) for p in trial.glob('*.csv')})
            if driver == 'CaptureProbe':
                beats = defaultdict(int)
                for row in rows(trial/'arrays.csv'):
                    if row['event'] == 'link_read':
                        beats[int(row['cycle'])] += int(row['bytes'])
                assert sum(beats.values()) == 512 and max(beats.values()) <= 16, beats
                result['aggregate_input_bytes'] = 512
                result['peak_global_input_bytes_per_cycle'] = max(beats.values())
                if enabled:
                    released = defaultdict(int)
                    for row in rows(trial/'array-requests.csv'):
                        if row['event'] == 'release':
                            array = int(row['array'])
                            released[array] = max(released[array], int(row['cycle']))
                    assert proof['capture_1'] == released[0], (proof, released)
                    assert proof['capture_2'] == released[1], (proof, released)
                    result['captured_on_final_timed_release'] = True
            results.append(result)
            (trial/'validation.json').write_text(json.dumps(result, indent=2)+'\n')
            print('PASS', enabled, driver, flush=True)
    assert before == {str(p): sha(p) for p in inputs}, 'Sources changed during validation'
    summary = dict(passed=True, configurations=len(results), results=results,
                   build=build_info, source_sha256=before)
    (output/'validation.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(f'PASS {len(results)} analog command backend regressions', flush=True)


if __name__ == '__main__':
    main()
