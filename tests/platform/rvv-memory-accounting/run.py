"""Exact vector counts and grant-size invariance across three memory boundaries."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0,str(ROOT/'tools/hardware'))
from hardware_paths import resolve_paths, require_owned_output
from hardware_runner import execute


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',type=Path,help='Explicit pinned binary for a negative reproduction')
    args = parser.parse_args()
    paths = resolve_paths()
    build,install = Path(paths['GOLEM_BUILD_ROOT']),Path(paths['GOLEM_INSTALL_ROOT'])
    output = build/'tests/platform/rvv-memory-accounting'/str(time.time_ns())
    require_owned_output(output,build)
    output.mkdir(parents=True,exist_ok=False)
    qemu = (args.qemu or install/'qemu/bin/qemu-system-riscv64').resolve()
    env = dict(os.environ,**paths,PYTHONDONTWRITEBYTECODE='1',
               SST_LIB_PATH=str(install/'sst-elements/lib/sst-elements-library'))
    build_result = execute(['make','-C',str(HERE),f'OUTPUT_DIR={output/"elf"}'],env,output/'build.log',60)
    if build_result['status'] != 'PASS':
        (output/'results.json').write_text(json.dumps(dict(status='BUILD_FAILED',build=build_result),indent=2)+'\n')
        return 1
    rows = []
    baseline = {}
    print(f'Accounting evidence: {output}',flush=True)
    for kernel in range(3):
        for quantum in (1,2,3,7,31,127,1000):
            trial = output/f'kernel{kernel}-q{quantum}'
            for directory in ('profile','tasks','serial'):
                (trial/directory).mkdir(parents=True)
            command = [str(install/'sst-core/bin/sst'),str(HERE/'simulation.py')]
            run_env = dict(env,ACCOUNTING_TRIAL=str(trial),ACCOUNTING_QUANTUM=str(quantum),
                           ACCOUNTING_QEMU=str(qemu),ACCOUNTING_ELF=str(output/'elf'/f'kernel{kernel}.elf'))
            row = dict(kernel=kernel,quantum=quantum,status='FAIL',command=command)
            try:
                row['simulation'] = execute(command,run_env,trial/'simulation.log',30)
                if row['simulation']['status'] != 'PASS':
                    row['status'] = row['simulation']['status']
                    raise ValueError('simulator failed; see preserved log')
                serial = (trial/'serial/tile-0.log').read_text()
                if serial.count('RVV_MEMORY_ACCOUNTING_PASS') != 1 or 'ACCOUNTING_FAIL' in serial:
                    raise ValueError('guest validation missing')
                doc = json.loads((trial/'profile/tile-0-summary.json').read_text())
                metrics = {r['name']:r['value'] if r['status']=='available' else None for r in doc['metrics']}
                expected = 131 if kernel != 2 else 67  # 64 bodies + vset + vmv + consumption.
                if metrics['vector_instructions'] != expected:
                    raise ValueError(f'expected {expected} vectors, got {metrics["vector_instructions"]}')
                if metrics['cpu_cycles'] != metrics['instructions']:
                    raise ValueError('one-issue cycles do not equal exact guest instruction count')
                with (trial/'tasks/tile-0.csv').open() as stream:
                    tasks = list(csv.DictReader(stream))
                if len(tasks)!=2 or [r['event'] for r in tasks]!=['start','finish']:
                    raise ValueError('unbalanced measured region')
                retired = int(tasks[1]['retired_instructions'])-int(tasks[0]['retired_instructions'])
                cycles = int(tasks[1]['cpu_cycles'])-int(tasks[0]['cpu_cycles'])
                if retired != cycles:
                    raise ValueError('measured one-issue region has inconsistent cycles')
                signature = (metrics['instructions'],metrics['vector_instructions'],retired,cycles)
                if baseline.setdefault(kernel,signature) != signature:
                    raise ValueError(f'grant-size dependent accounting: {signature} vs {baseline[kernel]}')
                row.update(status='PASS',total_instructions=signature[0],vectors=expected,
                           measured_instructions=retired,measured_cpu_cycles=cycles)
            except (OSError,ValueError,subprocess.SubprocessError) as error:
                row['error'] = str(error)
            rows.append(row)
            print(f'kernel={kernel} quantum={quantum}: {row["status"]} {row.get("error","")}',flush=True)
    result = dict(status='PASS' if all(r['status']=='PASS' for r in rows) else 'FAIL',cases=rows,
        qemu=str(qemu),qemu_sha256=hashlib.sha256(qemu.read_bytes()).hexdigest(),
        guest_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (output/'elf').glob('*.elf')})
    (output/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'{result["status"]}: {output/"results.json"}',flush=True)
    return result['status']!='PASS'


if __name__=='__main__':
    raise SystemExit(main())
