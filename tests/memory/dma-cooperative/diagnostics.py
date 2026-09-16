"""Repeated, provenance-checked waiting-policy and synchronization diagnostics."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def save(path, value):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2)+'\n')
    tmp.replace(path)

def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))

def execute(command, env, log):
    with log.open('w') as stream:
        process = subprocess.Popen(command, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
        if code:
            raise RuntimeError(f'simulation exit {code}: {log}')

def analyze(trial, case):
    serial = (trial/'serial/tile-0.log').read_text()
    assert 'DMA_COOPERATIVE_PASS' in serial, serial
    assert 'DMA_COOPERATIVE_PEER_PASS' in (trial/'serial/tile-1.log').read_text()
    tasks = rows(trial/'tasks/tile-0.csv')
    start = next(t for t in tasks if t['task_id']=='1' and t['event']=='start')
    end = next(t for t in tasks if t['task_id']=='1' and t['event']=='finish')
    start_tick, end_tick = int(start['sim_time_ticks']), int(end['sim_time_ticks'])
    requests = [r for r in rows(trial/'profile/global-ram-requests.csv') if r['execution_id']=='901']
    assert len(requests)==1 and int(requests[0]['byte_count'])==65536
    dma = requests[0]
    summary = {r['metric']:int(r['value']) for r in rows(trial/'profile/tile-0-summary.csv')}
    polls, pending = map(int,re.search(r'polls=(\d+) pending=(\d+)',serial).groups())
    network_path = trial/'profile/tile-0-network.csv'
    peer_path = trial/'profile/tile-1-network.csv'
    # Trace files are created lazily. No traffic is requested in the idle arm;
    # missing traces in a traffic arm are an error, not zero measurements.
    assert case['rounds']==0 or (network_path.exists() and peer_path.exists())
    network = rows(network_path) if network_path.exists() else []
    peer = rows(peer_path) if peer_path.exists() else []
    arrivals = [r for r in network if r['event']=='arrive']
    replies = [r for r in network if r['event']=='inject']
    received = [r for r in peer if r['event']=='arrive']
    assert len(arrivals)==len(replies)==len(received)==case['rounds']
    handling=[t for t in tasks if int(t['task_id'])>=1000]
    if handling:
        assert len(handling)==case['rounds']
        for request,handled,reply,receipt in zip(arrivals,handling,replies,received):
            assert int(request['event_tick'])<=int(handled['sim_time_ticks'])<=int(reply['event_tick'])<=int(receipt['event_tick'])
    timeline = dict(cpu_region=[start_tick,end_tick], dma=dma, requests=arrivals,
                    replies=replies, peer_received=received,
                    software_handling=handling,
                    cpu_waits=[r for r in rows(trial/'profile/tile-0-waits.csv')
                               if int(r['start_tick'])>=start_tick and int(r['finish_tick'])<=end_tick])
    save(trial/'timeline.json', timeline)
    first_issue = int(replies[0]['event_tick']) if replies else None
    result = dict(case, status='PASS', elapsed_cycles=(end_tick-start_tick)/1000,
        guest_cpu_cycles=int(end['cpu_cycles'])-int(start['cpu_cycles']),
        instructions=int(end['retired_instructions'])-int(start['retired_instructions']),
        polls=polls,pending_queries=pending,
        first_reply_issue_cycles=(first_issue-start_tick)/1000 if replies else None,
        first_response_arrival_cycles=(int(received[0]['event_tick'])-start_tick)/1000 if received else None,
        first_request_to_reply_cycles=(first_issue-int(arrivals[0]['event_tick']))/1000 if replies else None,
        reply_before_dma_completion=first_issue<int(dma['completion_cycle'])*1000 if replies else None,
        ram_service_cycles=int(dma['service_cycles']),ram_queue_cycles=int(dma['queue_cycles']),
        whole_run_bridge_events=summary.get('synchronization_events'),
        whole_run_grants=summary.get('synchronization_grants'),
        round_trips_per_cycle=case['rounds']/((end_tick-start_tick)/1000) if case['rounds'] else None,
        guest_sha256={p.name:digest(p) for p in Path(case['guest']).glob(f'm{case["mode"]}-*.elf')})
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install',type=Path,default=ROOT/'install/dma-event')
    parser.add_argument('--repeats',type=int,default=3)
    parser.add_argument('--quantums',type=int,nargs='+',default=[64,256,1000,4096])
    parser.add_argument('--suite',choices=['baseline','compute','event'],default='baseline')
    parser.add_argument('--replay',type=Path,help='Reuse an existing plan and its exact guest ELFs')
    parser.add_argument('--instruction-trace',action='store_true',help='Diagnostic-only instruction attribution; not for host speed comparisons')
    parser.add_argument('--router-trace',action='store_true',help='Record actual router output grants and tails')
    args = parser.parse_args()
    assert args.repeats>0 and all(q>0 for q in args.quantums)
    out = ROOT/'tests/results/dma-diagnostics'/str(time.time_ns())
    out.mkdir(parents=True)
    print(out,flush=True)
    install = args.install.resolve()
    env = dict(os.environ,GOLEM_INSTALL_ROOT=str(install),
               SST_LIB_PATH=str(install/'sst-elements/lib/sst-elements-library'))
    binaries = [install/'sst-core/bin/sst',install/'qemu/bin/qemu-system-riscv64',
                install/'sst-elements/lib/sst-elements-library/libmittens.so']
    hashes = {str(p):digest(p) for p in binaries}
    save(out/'provenance.json',dict(binaries=hashes,source={p.name:digest(p) for p in HERE.iterdir() if p.is_file()},
         timebase_seconds=1e-12,cpu_period_ticks=1000,seed=901))
    (out/'ram.bin').write_bytes(bytes(range(256))*256)
    save(out/'input.json',dict(payload_bytes=65536,ram_image_sha256=digest(out/'ram.bin'),
                               pattern='byte[i] = i modulo 256'))
    cases = []
    shapes = [(0,1,0),(64,1,0),(64,4,0),(64,16,0),(64,64,0),(64,256,0)]
    if args.suite=='compute': shapes=[(0,1,n) for n in [0,128,512,2048,8192]]
    if args.suite=='event': shapes=[(0,1,0),(64,1,0)]
    if args.replay: shapes=[]
    for rounds,interval,work in shapes:
        guest = out/f'guest-r{rounds}-p{interval}-w{work}'
        subprocess.run(['bash',str(HERE/'build.sh'),str(guest)],env=dict(env,
            DMA_QUERY_ROUNDS=str(rounds),DMA_QUERY_INTERVAL=str(interval),DMA_QUERY_WORK=str(work),
            DMA_QUERY_DETAILED=str(int(args.instruction_trace))),check=True,
            stdout=subprocess.DEVNULL)
        modes = [0,1,3] if args.suite=='event' else ([0,1] if interval==1 else [1])
        for mode in modes:
            for quantum in ([1000] if args.suite=='compute' else args.quantums if interval==1 else [1000]):
                for repeat in range(args.repeats):
                    cases.append(dict(rounds=rounds,interval=interval,work_iterations=work,
                                      suite=args.suite,mode=mode,quantum=quantum,
                                      repeat=repeat,guest=str(guest)))
    if args.replay:
        cases=json.loads(args.replay.read_text())
        save(out/'replay.json',dict(plan=str(args.replay.resolve()),sha256=digest(args.replay)))
    else:
        random.Random(901).shuffle(cases)
    save(out/'plan.json',cases)
    results=[]
    for index,case in enumerate(cases):
        trial=out/f'case-{index:03}'
        trial.mkdir()
        for folder in ('serial','profile','tasks'): (trial/folder).mkdir()
        save(trial/'case.json',case)
        start=time.monotonic()
        load_start=os.getloadavg()
        try:
            case_env=dict(env,DMA_QUERY_CASE=str(trial),
                DMA_QUERY_MODE=str(case['mode']),DMA_QUERY_GUEST=case['guest'],
                DMA_QUERY_QUANTUM=str(case['quantum']))
            if args.instruction_trace: case_env['MITTENS_CPU_TRACE_DIRECTORY']=str(trial/'profile')
            if args.router_trace: case_env['DMA_QUERY_ROUTER_TRACE']='1'
            execute([str(binaries[0]),str(HERE/'simulation.py')],case_env,trial/'run.log')
            wall=time.monotonic()-start
            assert hashes=={str(p):digest(p) for p in binaries}, 'simulator changed'
            result=analyze(trial,case)
            result.update(host_wall_seconds=wall,directory=str(trial),host_load_start=load_start,
                          host_load_finish=os.getloadavg(),instruction_trace=args.instruction_trace,
                          router_trace=args.router_trace)
        except Exception as error:
            result=dict(case,status='FAIL',error=str(error),directory=str(trial))
            save(trial/'result.json',result)
            save(out/'results.json',results+[result])
            raise
        results.append(result)
        save(trial/'result.json',result)
        save(out/'results.json',results)
        print(f'{index+1}/{len(cases)} '+json.dumps(result),flush=True)
    save(out/'complete.json',dict(status='PASS',cases=len(results)))

if __name__=='__main__':
    main()
