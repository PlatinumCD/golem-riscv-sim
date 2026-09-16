"""Blocking versus cooperative global DMA, with identical two-tile hardware."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', type=Path, default=ROOT/'install/dma-event')
    args = parser.parse_args()
    out = ROOT/'tests/results/dma-cooperative'/str(time.time_ns())
    out.mkdir(parents=True)
    print(out, flush=True)
    env = dict(os.environ, GOLEM_INSTALL_ROOT=str(args.install.resolve()),
               SST_LIB_PATH=str(args.install.resolve()/'sst-elements/lib/sst-elements-library'))
    subprocess.run(['bash', str(HERE/'build.sh'), str(out/'guest')], env=env, check=True)
    artifacts = [args.install/'qemu/bin/qemu-system-riscv64',
                 args.install/'sst-elements/lib/sst-elements-library/libmittens.so']
    hashes = {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (out/'provenance.json').write_text(json.dumps(hashes,indent=2)+'\n')
    (out/'ram.bin').write_bytes(bytes(range(256))*256)
    results = []
    for mode in (0, 1, 2, 3):
        trial = out/('blocking', 'cooperative', 'cooperative-every-16', 'event-driven')[mode]
        trial.mkdir()
        for directory in ('serial', 'tasks', 'profile'):
            (trial/directory).mkdir()
        start = time.monotonic()
        with (trial/'run.log').open('w') as log:
            subprocess.run([str(args.install/'sst-core/bin/sst'), str(HERE/'simulation.py')],
                env=dict(env,DMA_QUERY_MODE=str(mode),DMA_QUERY_CASE=str(trial)),
                stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
        wall = time.monotonic()-start
        assert hashes == {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}, 'Simulator changed during comparison'
        serial = (trial/'serial/tile-0.log').read_text()
        assert 'DMA_COOPERATIVE_PASS' in serial, serial
        assert 'DMA_COOPERATIVE_PEER_PASS' in (trial/'serial/tile-1.log').read_text()
        task = rows(trial/'tasks/tile-0.csv')
        begin = int(task[0]['sim_time_ticks'])
        first = int(task[1]['sim_time_ticks'])
        finish = int(task[2]['sim_time_ticks'])
        ram = [r for r in rows(trial/'profile/global-ram-requests.csv') if int(r['execution_id']) == 901]
        assert len(ram)==1 and int(ram[0]['byte_count'])==65536
        dma_finish = int(ram[0]['completion_cycle'])*1000
        assert (first < dma_finish) == bool(mode), (first,dma_finish)
        polls,pending = map(int,re.search(r'polls=(\d+) pending=(\d+)',serial).groups())
        result = dict(mode=trial.name, elapsed_cycles=(finish-begin)//1000,
                      guest_cpu_cycles=int(task[2]['cpu_cycles'])-int(task[0]['cpu_cycles']),
                      retired_instructions=int(task[2]['retired_instructions'])-int(task[0]['retired_instructions']),
                      first_reply_cycles=(first-begin)//1000,
                      reply_before_ram_completion=first<dma_finish,
                      polls=polls,pending_queries=pending,host_wall_seconds=wall,
                      application_round_trips=64,
                      round_trips_per_cycle=64/((finish-begin)/1000),correctness='PASS')
        results.append(result)
        (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps(result),flush=True)
    print('Application throughput speedup:',results[0]['elapsed_cycles']/results[1]['elapsed_cycles'])

if __name__ == '__main__':
    main()
