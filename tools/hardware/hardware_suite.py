"""Explicit hardware coverage shared by the normal runner and baseline comparisons."""
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Case:
    name: str
    script: Path
    measurements_required: bool = False

    def command(self):
        return (['python3', '-B'] if self.script.suffix == '.py' else ['bash']) + [str(self.script)]


HARDWARE = {
    'platform': ('hello', 'riscv-vector', 'cpu-timing', 'register-only-accounting', 'rvv-only-accounting', 'rvv-memory-accounting'),
    'memory': ('global-ram', 'global-ram-exact', 'scratchpad-dma', 'global-dma-macro-contention'),
    'network': ('pair', 'mesh-3x3', 'timing', 'transmit-fanout', 'pipeline', 'communication-envelope'),
    'runtime': ('library', 'deployment-pair', 'epoch-barrier'),
    'analog': ('instructions', 'ops', 'timing', 'mesh-2x2', 'route-2x2',
               'mesh-2x2-dual-array', 'distributed-matvec', 'producer-mvm-recombine-distance'),
}
GROUPS = (*HARDWARE, 'compiler', 'models', 'validation')


def hardware_cases():
    cases = [Case('host', ROOT / 'tools/hardware/verify.py'),
             Case('configuration', ROOT / 'src/sst/tests/run-configuration-test.py'),
             Case('component', ROOT / 'src/sst/tests/run-test.sh', True),
             Case('tx-controller', ROOT / 'src/sst/tests/tx_controller_regression.py', True),
             Case('rx-controller', ROOT / 'src/sst/tests/rx_controller_regression.sh', True)]
    for group, names in HARDWARE.items():
        for name in names:
            directory = ROOT / 'tests' / group / name
            script = directory / ('run-all.sh' if name == 'deployment-pair' else 'run-test.sh')
            cases.append(Case(f'{group}/{name}', script))
    cases.append(Case('validation/performance-profile', ROOT / 'tests/validation/performance-profile/run-test.sh'))
    return cases


def selected_cases(suite='hardware', names=None, group=None):
    if group is not None:
        if group not in GROUPS or suite != 'hardware':
            raise ValueError('select one known group or one suite, not both')
        # Hardware groups use the full suite's allowlist. Other discovery is opt-in.
        cases = [case for case in hardware_cases() if case.name.startswith(group + '/')]
        groups = () if group in HARDWARE else (group,)
    else:
        cases = hardware_cases() if suite in ('hardware', 'all') else []
        groups = ('compiler', 'models', 'validation') if suite == 'all' else ((suite,) if suite != 'hardware' else ())
    existing = {case.name for case in cases}
    for discovered_group in groups:
        for directory in sorted((ROOT / 'tests' / discovered_group).iterdir()):
            name = f'{discovered_group}/{directory.name}'
            if not directory.is_dir() or name in existing:
                continue
            for entry in ('run-all.sh', 'run-test.sh'):
                if (directory / entry).is_file():
                    cases.append(Case(name, directory / entry))
                    break
    if names:
        by_name = {case.name: case for case in cases}
        unknown = set(names) - by_name.keys()
        if unknown:
            raise ValueError(f'cases not in {group or suite} selection: {sorted(unknown)}')
        cases = [by_name[name] for name in dict.fromkeys(names)]
    return cases
