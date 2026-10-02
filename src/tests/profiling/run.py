"""Verify passive cycle profiling on guest MVM → multi-hop message → MVM."""
import argparse
from collections import Counter, defaultdict
from copy import deepcopy
import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
FIXTURE = HERE.parent / 'network-instructions'
sys.path[:0] = [str(SOURCE), str(FIXTURE)]
from build import load_build_info

spec = importlib.util.spec_from_file_location('profile_mesh_fixture', FIXTURE / 'run.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def write(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def audit(trial):
    directory = trial / 'profiles'
    files = list(directory.glob('*-cycles.csv'))
    assert len(files) == 40, ('Missing component/port profiles', len(files))
    sends, receives, port_cycles = defaultdict(list), defaultdict(list), Counter()
    seen = set()
    for path in files:
        with path.open() as stream:
            reader = csv.DictReader(stream)
            assert reader.fieldnames == ['cycle', 'kind', 'resource', 'index', 'value', 'token', 'detail']
            records = list(reader)
        assert records, ('Empty profile', path.name)
        cycles = [int(row['cycle']) for row in records]
        assert cycles == sorted(cycles), ('Nonmonotonic timestamps', path.name)
        for row in records:
            seen.add(row['resource'])
            if row['resource'] not in ('flit_send', 'flit_receive'):
                continue
            key = tuple(int(row[name]) for name in ('detail', 'token', 'value'))
            cycle = int(row['cycle'])
            if row['resource'] == 'flit_send':
                sends[key].append(cycle)
                port_cycles[path.name, cycle] += 1
            else:
                receives[key].append(cycle)
    assert sends and sends.keys() == receives.keys(), 'Lost network flits'
    for key, sent in sends.items():
        received = receives[key]
        assert len(sent) == len(received), ('Unmatched flit hops', key)
        assert all(b - a == 1 for a, b in zip(sorted(sent), sorted(received))), ('Wrong link delay', key)
    assert max(port_cycles.values()) == 1, 'A port exceeded one flit/cycle'
    assert {'command_queue', 'compute_active', 'result_slots', 'memory_fragments',
            'pending_messages', 'active_byte_ranges', 'requests_awaiting_service',
            'tx_flits', 'rx_packets', 'router_credits'} <= seen, ('Missing resource observations', seen)
    for tile in range(4):
        for suffix in ('icache', 'memory', 'lsq', 'slq', 'asq', 'waits', 'slq-waits', 'asq-waits'):
            path = trial / f'net.tile{tile}.riscv-{suffix}.csv'
            assert path.is_file() and path.stat().st_size > 0, ('Missing CPU trace', path)
    return dict(component_profiles=len(files), distinct_flits=len(sends),
                observed_link_hops=sum(map(len, sends.values())),
                exact_one_cycle_links=True, one_flit_per_port_per_cycle=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--build-info', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'install/llvm/bin/clang')
    args = parser.parse_args()
    info = load_build_info(args.build_info)
    output = (args.output or ROOT / 'tests/results/profiling' / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    case = fixture.configure(next(c for c in fixture.cases() if c['name'] == 'mvm-multi-hop-mvm'), args.qemu)
    guests = output / 'guests'
    guests.mkdir()
    case['elfs'] = fixture.compile_guests(guests, case, args.compiler.resolve())
    measurements, profiles = {}, {}
    for name, flag, budget in (('default-off', None, 256), ('explicit-off', '0', 256),
                              ('on', '1', 256), ('on-budget1', '1', 1)):
        trial = output / name
        trial.mkdir()
        current = deepcopy(case)
        current.update(name=name, profile=flag == '1')
        current['cpu_parameters']['instruction_budget'] = budget
        write(trial / 'case.json', current)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('TILE_CYCLE_PROFILE', 'TILE_COMPONENT_'))
               and k not in ('SST_LIB_PATH', 'TILE_PROFILE_SPM_CONNECTIONS')}
        env.update(TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1',
                   TILE_CYCLE_PROFILE_DIRECTORY=str(trial / 'profiles'))
        if flag is not None:
            env['TILE_CYCLE_PROFILE'] = flag
        command = [info['sst'], '--num-threads=1', f'--add-lib-path={info["plugin"]}',
                   f'--output-json={trial / "topology.json"}', str(FIXTURE / 'simulation.py')]
        with (trial / 'simulation.log').open('w') as stream:
            subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                           timeout=180, check=True)
        checks = fixture.validate(trial, current)
        measurements[name] = checks
        write(trial / 'validation.json', dict(passed=True, checks=checks))
        if flag == '1':
            profiles[name] = audit(trial)
        else:
            assert not (trial / 'profiles').exists(), 'Disabled profiling created output'
        print(f'PASS profiling {name}', flush=True)
    baseline = output / 'default-off'
    for name in ('explicit-off', 'on'):
        assert measurements[name] == measurements['default-off'], ('Profiling changed behavior', name)
        for path in baseline.glob('*.csv'):
            assert path.read_bytes() == (output / name / path.name).read_bytes(), ('Event trace changed', name, path.name)
        for path in baseline.glob('tile*-spm.bin'):
            assert path.read_bytes() == (output / name / path.name).read_bytes(), ('SPM changed', name, path.name)
    # Synchronization grant size only changes host rendezvous bookkeeping.
    normal, single = deepcopy(measurements['on']), deepcopy(measurements['on-budget1'])
    for record in (normal, single):
        for cpu in record['cpu']:
            cpu.pop('grants', None)
            cpu.pop('stops', None)
    assert normal == single, 'Instruction budget changed architectural timing/traffic'
    write(output / 'validation.json', dict(passed=True, cases=4,
        default_off=True, explicit_off=True, identical_profile_on_off_timing_and_values=True,
        identical_event_traces=True, identical_instruction_budget_timing=True, profiles=profiles))
    print(f'PASS profiling regression: {output}', flush=True)


if __name__ == '__main__':
    main()
