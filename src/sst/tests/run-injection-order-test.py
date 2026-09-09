#!/usr/bin/env python3
"""Check deferred source-router credits wake an otherwise empty real NIC."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT/'tools/hardware'))
from hardware_paths import require_owned_output, resolve_paths
from hardware_runner import execute


def main():
    paths = resolve_paths()
    build, install = Path(paths['GOLEM_BUILD_ROOT']), Path(paths['GOLEM_INSTALL_ROOT'])
    output = build/'tests/injection-order'/str(time.time_ns())
    require_owned_output(output, build)
    output.mkdir(parents=True, exist_ok=False)
    config = install/'sst-core/bin/sst-config'
    flags = shlex.split(subprocess.check_output([str(config), '--ELEMENT_CXXFLAGS'], text=True))
    command = [os.environ.get('HOST_CXX','c++'), *flags, '-shared',
               '-I'+str(HERE.parent), str(HERE/'injection_order_credit_probe.cpp'),
               '-o', str(output/'libinjection_credit_test.so')]
    env = dict(os.environ, **paths, PYTHONDONTWRITEBYTECODE='1',
               SST_LIB_PATH=f'{output}:{install}/sst-elements/lib/sst-elements-library')
    record = {'status':'FAIL', 'build':execute(command,env,output/'build.log',60)}
    if record['build']['status']=='PASS':
        record['simulation'] = execute([str(install/'sst-core/bin/sst'),
            str(HERE/'injection_order_credit.py')],env,output/'simulation.log',30)
        record['status'] = record['simulation']['status']
        if record['status']=='PASS' and 'injection credit wakeup: PASS' not in (output/'simulation.log').read_text():
            record.update(status='FAIL',error='missing migration and wakeup proof')
    else:
        record['status']='BUILD_FAILED'
    (output/'results.json').write_text(json.dumps(record,indent=2)+'\n')
    print(f'Injection ordering credit wakeup: {record["status"]}; {output/"results.json"}',flush=True)
    return record['status']!='PASS'


if __name__=='__main__':
    raise SystemExit(main())
