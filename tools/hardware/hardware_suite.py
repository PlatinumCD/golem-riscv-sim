"""Explicit correctness coverage for the current component model."""
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'src'
GROUPS = ('platform', 'memory', 'analog', 'network', 'compiler')


@dataclass(frozen=True)
class Case:
    name: str
    script: Path
    group: str
    build_info: bool = False
    qemu: bool = False
    arguments: tuple = ()


def hardware_cases():
    def case(name, folder, group, build_info=True, qemu=False, arguments=()):
        return Case(name, SOURCE / 'tests' / folder / 'run.py', group, build_info, qemu, arguments)
    return [
        Case('host', ROOT / 'tools/hardware/verify.py', 'platform'),
        Case('component', SOURCE / 'tests/run.py', 'memory', True),
        case('platform/riscv-qemu', 'riscv-qemu', 'platform', qemu=True),
        case('platform/instruction-cache', 'instruction-cache', 'platform', qemu=True),
        case('platform/vector-memory', 'vector-memory', 'platform', qemu=True),
        case('platform/load-store-queue', 'load-store-queue', 'platform', qemu=True),
        case('platform/compressed-scalar', 'compressed-scalar', 'platform', arguments=('--variants', 'after')),
        case('platform/llvm-rvv', 'llvm-rvv', 'platform', qemu=True),
        case('platform/profiling', 'profiling', 'platform', qemu=True),
        case('memory/range-ordering', 'range-ordering', 'memory'),
        case('memory/bank-connections', 'bank-connections', 'memory'),
        case('analog/vector-analog', 'vector-analog', 'analog', qemu=True),
        case('analog/array-pipeline', 'array-pipeline', 'analog', qemu=True),
        case('analog/programming-delay', 'programming-delay', 'analog', qemu=True, arguments=('--skip-legacy',)),
        case('analog/register-dependencies', 'analog-register-dependencies', 'analog', qemu=True),
        case('analog/command-queue', 'analog-command-queue', 'analog', qemu=True, arguments=('--candidate-only',)),
        case('analog/command-admission', 'analog-command-queue', 'analog', qemu=True, arguments=()),
        case('analog/command-backend', 'analog-command-queue-backend', 'analog', build_info=False),
        case('network/mesh-2x2', 'mordred', 'network', build_info=False),
        case('network/mordred-spm', 'mordred-spm', 'network', qemu=True),
        case('network/local-spm', 'mordred-local', 'network'),
        case('network/posted-transfers', 'mordred-posted', 'network'),
    ]


def selected_cases(suite='hardware', names=None, group=None):
    if suite not in ('hardware', 'compiler', 'all'):
        raise ValueError('Supported suites: hardware, compiler, all. Legacy suites are retired; see docs/migration.md.')
    cases = hardware_cases() if suite in ('hardware', 'all') else []
    if suite in ('compiler', 'all'):
        cases += [Case('compiler/sculptor', SOURCE / 'tests/sculptor/run.py', 'compiler', True, True)]
    if group:
        if group not in GROUPS:
            raise ValueError(f'Unknown group: {group}')
        cases = [c for c in cases if c.group == group]
    if names:
        by_name = {c.name: c for c in cases}
        unknown = set(names) - by_name.keys()
        if unknown:
            raise ValueError(f'Unknown or retired cases: {sorted(unknown)}. Use --list; see docs/migration.md.')
        cases = [by_name[name] for name in dict.fromkeys(names)]
    return cases
