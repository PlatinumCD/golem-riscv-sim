"""Check selective analog register waits, precise traps, and host-budget invariance."""
import argparse
from collections import Counter, deque
import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
ROOT = SOURCE.parent
sys.path.insert(0, str(SOURCE))
from configuration import resolve


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def stats(log, label):
    found = [json.loads(line[len(label)+1:]) for line in log.splitlines() if line.startswith(label+' ')]
    assert len(found) == 1, (label, found)
    return found[0]


def compile_guest(output, compiler, vlen):
    support = SOURCE / 'tests/riscv-qemu'
    n = vlen // 4
    data = output / f'data-{vlen}.S'
    parts = ['.section .rodata,"a",@progbits']
    for name, values in [('weights', (float(i == j) for i in range(n) for j in range(n))),
                         ('input_data', range(1, n+1)), ('noise_data', range(1000, 1000+n))]:
        parts += ['.balign 4096', '.global '+name, name+':']
        words = [str(struct.unpack('<I', struct.pack('<f', value))[0]) for value in values]
        parts += ['.word '+','.join(words[i:i+16]) for i in range(0,len(words),16)]
    data.write_text('\n'.join(parts)+'\n')
    elf = output / f'guest-{vlen}.elf'
    command = [str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog', '-fuse-ld=lld',
               '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0', '-nostdlib', '-static',
               '-Wl,--no-relax', '-Wl,--build-id=none', f'-DVLEN_BITS={vlen}',
               '-T', str(support/'scratchpad.ld'), str(support/'start.S'),
               str(SOURCE/'tests/array-pipeline/measurement.S'), str(HERE/'directed.S'), str(data),
               '-o', str(elf)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=90)
    elf.with_suffix('.build.log').write_text(process.stdout+process.stderr)
    assert process.returncode == 0, process.stderr
    elf.with_suffix('.asm').write_text(subprocess.check_output([str(compiler.parent/'llvm-objdump'), '-d', str(elf)],text=True))
    symbols = {}
    for line in subprocess.check_output([str(compiler.parent/'llvm-nm'), '--defined-only', str(elf)],text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    result = dict(elf=str(elf), sha256=digest(elf), symbols=symbols, command=command)
    dump(elf.with_suffix('.json'), result)
    return result


def load_model(name, build_path, qemu):
    build_path, qemu = build_path.resolve(), qemu.resolve()
    build = json.loads(build_path.read_text())
    if name == 'current':
        for path, expected in build['source_sha256'].items():
            assert digest(path) == expected, ('Source changed after build', path)
    # Historical baseline sources may have been replaced during promotion. Its
    # original build manifest and the actual binary hashes remain recorded.
    return dict(name=name, build=build, build_info=str(build_path), qemu=str(qemu),
                source_fingerprints_verified=name == 'current',
                qemu_sha256=digest(qemu),
                plugin_sha256=digest(Path(build['plugin'])/'libtilecomponents.so'))


def run(output, guest, model, vlen, depth, budget):
    name = f"{model['name']}-v{vlen}-d{depth}-budget{budget}"
    trial = output/name
    trial.mkdir()
    build = model['build']
    case = dict(name=name, model=model['name'], vlen=vlen, depth=depth, budget=budget, **guest,
                qemu=model['qemu'], qemu_sha256=model['qemu_sha256'],
                plugin_sha256=model['plugin_sha256'], build_info=model['build_info'],
                parameters=resolve(dict(riscv_vector_length_bits=vlen, array_rows=vlen//4,
                    array_cols=vlen//4, spm_banks=1, array_pipeline_enabled=False,
                    array_inflight_bytes=128)),
                cpu_parameters=dict(load_store_queue_depth=depth, instruction_budget=budget,
                                    host_timeout_seconds=120))
    dump(trial/'case.json', case)
    environment = os.environ | dict(SST_LIB_PATH=build['plugin']+':'+build['library'],
                                   TILE_COMPONENT_OUTPUT=str(trial), PYTHONDONTWRITEBYTECODE='1')
    # Directed assertions need setup and measured traces alike.
    environment.pop('TILE_COMPONENT_TRACE_START_TASK', None)
    environment.pop('TILE_COMPONENT_PROGRAM_PROOF', None)
    with (trial/'simulation.log').open('w') as log:
        process = subprocess.Popen([build['sst'], '--num-threads=1',
            f"--output-json={trial/'topology.json'}", str(HERE/'simulation.py')],
            env=environment,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=180)
        finally:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()
    log = (trial/'simulation.log').read_text()
    assert process.returncode == 0, (name, log[-8000:])
    return trial, case, log


def check_lsq(trial, case, cpu):
    active, entries, order = {}, {}, deque()
    stalls = Counter()
    peak = 0
    previous = -1
    for row in rows(trial/'riscv-lsq.csv'):
        event, reason = row['event'], row['reason']
        d = {k:int(v) for k,v in row.items() if k not in ('event','reason')}
        token, cycle = d['token'],d['cycle']
        assert cycle >= previous, row
        previous = cycle
        if event == 'stall':
            assert reason in ('register','full','drain'), row
            stalls[reason] += 1
        elif event == 'enqueue':
            assert token not in entries and d['slot'] not in {e['slot'] for e in active.values()}, row
            assert 0 <= d['slot'] < case['depth'] and 0 < d['bytes'] <= case['vlen']//8, row
            entries[token] = active[token] = d | dict(enqueue=cycle)
            order.append(token)
            peak = max(peak,len(active))
        else:
            assert token in active and event in ('issue','service_complete','complete'), row
            entry = active[token]
            assert all(entry[k] == d[k] for k in ('slot','pc','address','bytes','write')), row
            assert event not in entry, row
            entry[event] = cycle
            if event == 'issue': assert cycle == entry['enqueue'], row
            elif event == 'service_complete': assert cycle > entry['issue'], row
            else:
                assert cycle >= entry['service_complete'] and order.popleft() == token, row
                del active[token]
        assert d['occupancy'] == len(active) <= case['depth'], row
    assert not active and not order
    assert len(entries) == cpu['lsq_enqueued'] == cpu['lsq_completed'], cpu
    assert peak == cpu['lsq_peak_occupancy'], cpu
    for reason in ('register','full','drain'):
        assert stalls[reason] == cpu[f'lsq_{reason}_stalls'], (stalls,cpu)
    if case['depth'] == 1: assert not entries and not stalls
    return list(entries.values()),dict(stalls)


def check_outputs(trial, case):
    l = case['vlen']//32
    n = l*8
    x = list(range(1,n+1))
    noise = list(range(1000,1000+n))
    modified = x[:-l]+noise[3*l:4*l]
    short = noise.copy()
    short[0] = 1
    short[-l:] = noise[3*l:4*l]
    expected = {0:x,1:noise,2:noise,3:x,4:noise,5:x,6:x,7:noise,8:noise,9:x,
                10:noise[:l],11:modified,12:x[:l//2]+noise[l//2:l],13:noise[:l],
                14:short,15:noise,16:x,17:noise[:l],19:x[:l],20:noise[:l]}
    memory = (trial/'scratchpad.bin').read_bytes()
    checked = 0
    for slot, values in expected.items():
        wanted = struct.pack(f'<{len(values)}f',*values)
        offset = 0x110000+4096*slot
        actual = memory[offset:offset+len(wanted)]
        assert actual == wanted, ('output mismatch',case['name'],slot,
                list(struct.unpack(f'<{len(values)}f',actual)),values)
        assert memory[offset+len(wanted):offset+4096] == bytes(4096-len(wanted)), ('output tail',slot)
        checked += len(values)
    assert struct.unpack_from('<I',memory,0x1e0000)[0] == 3, 'trap count'
    for index,phase in enumerate((10,11,12)):
        offset = 0x150000+index*4096
        assert memory[offset:offset+4*l] == struct.pack(f'<{l}f',*noise[:l]), ('trap loaded register',phase)
        cause,pc,oldstore = struct.unpack_from('<QQI',memory,offset+128)
        assert (cause,pc,oldstore) == (2,case['symbols'][f'target_p{phase}'],struct.unpack('<I',struct.pack('<f',1000))[0]), ('trap record',phase,cause,pc,oldstore)
        checked += l+1
    return checked


def canonical_trace_hashes(trial):
    """Only bridge token identity may change with host grant frequency.

    Use one increasing, bijective mapping across all analog trace files. Every
    event, ordering, timestamp, address, size and remaining field is preserved.
    Non-analog CSVs are compared byte for byte.
    """
    analog = {p.name:rows(p) for p in trial.glob('array*.csv')}
    tokens = sorted({int(r['token']) for rs in analog.values() for r in rs if 'token' in r})
    mapping = {token:index for index,token in enumerate(tokens)}
    hashes = {}
    for path in trial.glob('*.csv'):
        if path.name not in analog:
            hashes[path.name] = digest(path)
            continue
        canonical = [{k:(mapping[int(v)] if k == 'token' else v) for k,v in r.items()}
                     for r in analog[path.name]]
        hashes[path.name] = hashlib.sha256(json.dumps(canonical,separators=(',',':')).encode()).hexdigest()
    return hashes


def validate(trial, case, log):
    cpu = stats(log,'RISCV_STATS')
    assert cpu['memory_requests'] == cpu['completed_requests'], cpu
    entries,stalls = check_lsq(trial,case,cpu)
    checked = check_outputs(trial,case)
    arrays = stats(log,'ARRAY_STATS')
    assert arrays['accepted'] == arrays['completed'] == cpu['analog_commands'], arrays
    assert arrays['errors'] == 1 and arrays['busy'] == 0 and arrays['mvms'] == 5, arrays
    markers = rows(trial/'riscv-tasks.csv')
    timeline = rows(trial/'arrays.csv')
    fetches = rows(trial/'riscv-icache.csv')
    lsq = rows(trial/'riscv-lsq.csv')
    assert len(markers) == 24, markers
    phases = {}
    operations = {1:0,2:1,3:1,4:3,5:3,6:1,7:3,8:3,9:2}
    for phase in range(1,13):
        selected = [r for r in markers if int(r['task_id']) == phase]
        assert [r['event'] for r in selected] == ['start','finish'], selected
        for row in selected:
            assert row['memory_requests'] == row['completed_requests'], ('marker outstanding memory',row)
            assert row['lsq_enqueued'] == row['lsq_completed'], ('marker outstanding LSQ',row)
        lo,hi = [int(r['cycle']) for r in selected]
        target = case['symbols'][f'target_p{phase}']
        lookup = [int(r['cycle']) for r in fetches if r['event'] in ('hit','miss') and int(r['address']) == target]
        assert len(lookup) == 1 and lo <= lookup[0] <= hi, (phase,lookup)
        info = dict(start=lo,finish=hi,cycles=hi-lo,target_lookup=lookup[0])
        directed = {name:[e for e in entries if e['pc'] == pc]
                    for name,pc in case['symbols'].items() if name.startswith(f'issue_p{phase}_')}
        if phase <= 9:
            starts = [r for r in timeline if r['event'] == 'start' and lo <= int(r['cycle']) <= hi]
            assert len(starts) == 1 and int(starts[0]['operation']) == operations[phase], (phase,starts)
            info['array_start'] = start = int(starts[0]['cycle'])
            assert start >= lookup[0], (phase,start,lookup)
        if case['depth'] > 1:
            assert all(directed.values()), (phase,directed)
            info['memory'] = {}
            for name, es in directed.items():
                assert all(lo <= e['enqueue'] <= e['complete'] <= hi for e in es), (phase,name,es)
                info['memory'][name] = dict(beats=len(es), enqueue=min(e['enqueue'] for e in es),
                    complete=max(e['complete'] for e in es))
                if name.endswith('_related'):
                    assert max(e['complete'] for e in es) <= lookup[0] <= start, ('RAW/WAW violated',phase,es,info)
                    if case['model'] != 'baseline':
                        waits = [r for r in lsq if r['event'] == 'stall' and r['reason'] == 'register'
                                 and max(e['enqueue'] for e in es) <= int(r['cycle']) < max(e['complete'] for e in es)]
                        assert waits, ('Test did not exercise a pending register hazard',phase,info)
                if phase in (1,2,3,4,5,9) and (name.endswith('_unrelated') or name.endswith('_store')):
                    last = max(e['complete'] for e in es)
                    info['overlap_cycles'] = max(0,last-start)
                    if case['model'] != 'baseline':
                        assert start < last, ('Current model still drains unrelated memory',phase,info)
                    else:
                        assert last <= lookup[0] <= start, ('Baseline did not drain',phase,info)
        if phase >= 10:
            handlers = [int(r['cycle']) for r in fetches if r['event'] in ('hit','miss')
                        and int(r['address']) == case['symbols']['trap_handler'] and lo <= int(r['cycle']) <= hi]
            assert len(handlers) == 1, (phase,handlers)
            info['handler_lookup'] = handlers[0]
            assert lookup[0] <= handlers[0], (phase,info)
            for es in directed.values():
                assert all(e['complete'] <= handlers[0] for e in es), ('Trap observed older memory before commit',phase,info)
                if case['depth'] > 1 and case['model'] != 'baseline' and phase in (10,11):
                    assert all(e['enqueue'] <= lookup[0] < e['complete'] for e in es), (
                        'Dynamic trap did not occur with both older load/store still pending',phase,info,es)
                if phase == 12:
                    assert all(e['complete'] <= lookup[0] for e in es), ('Invalid predecode must retain conservative drain',phase,info)
        phases[str(phase)] = info
    result = dict(name=case['name'], cpu=cpu, stalls=stalls, outputs_checked=checked,
                  phases=phases, array_stats=arrays, memory_sha256=digest(trial/'scratchpad.bin'),
                  canonical_trace_hashes=canonical_trace_hashes(trial),
                  trace_hashes={p.name:digest(p) for p in trial.glob('*.csv')})
    dump(trial/'validation.json',result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--compiler',type=Path,default=ROOT/'install/llvm/bin/clang')
    parser.add_argument('--build-info',type=Path,default=ROOT/'build/src/components/build.json')
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--baseline-build-info',type=Path,help='Optional historical model with global analog drains')
    parser.add_argument('--baseline-qemu',type=Path,help='QEMU paired with --baseline-build-info')
    parser.add_argument('--vlen',type=int,action='append',choices=(128,256,1024),
                        help='Restrict depth-16 widths; VLEN 256 controls always run')
    parser.add_argument('--check-only',action='store_true',help='Recheck existing output without running SST')
    args=parser.parse_args()
    if bool(args.baseline_build_info) != bool(args.baseline_qemu):
        parser.error('--baseline-build-info and --baseline-qemu must be supplied together')
    if args.check_only and (args.output is None or not args.output.is_dir()):
        parser.error('--check-only requires an existing --output directory')
    output=(args.output or ROOT/'tests/results/source-new-analog-register-dependencies'/str(time.time_ns())).resolve()
    if not args.check_only:
        output.mkdir(parents=True,exist_ok=False)
    print(output,flush=True)
    results=[]
    variants=[]
    if args.check_only:
        for casefile in sorted(output.glob('*/case.json')):
            case=json.loads(casefile.read_text())
            assert digest(case['elf']) == case['sha256'], 'Guest ELF changed since execution'
            variants.append((case['vlen'],case['model'],case['depth'],case['budget']))
            results.append(validate(casefile.parent,case,(casefile.parent/'simulation.log').read_text()))
            print('PASS',case['name'],flush=True)
        dump(output/'results.json',results)
    else:
        models={'current':load_model('current',args.build_info,args.qemu)}
        if args.baseline_build_info:
            models['baseline']=load_model('baseline',args.baseline_build_info,args.baseline_qemu)
        widths=sorted(set(args.vlen or (128,256,1024)) | {256})
        variants=[(v,name,16,256) for v in widths for name in models]
        variants += [(256,name,1,256) for name in models]
        variants += [(256,'current',16,1)]
        guests={v:compile_guest(output,args.compiler.resolve(),v) for v in widths}
        dump(output/'metadata.json',dict(sources={str(p):digest(p) for p in HERE.iterdir() if p.is_file()},
            guests=list(guests.values()),models=models))
        for vlen,model,depth,budget in variants:
            trial,case,log=run(output,guests[vlen],models[model],vlen,depth,budget)
            result=validate(trial,case,log)
            results.append(result)
            dump(output/'results.json',results)
            print('PASS',case['name'],flush=True)
    assert results, 'No retained cases found to validate'
    byname={r['name']:r for r in results}
    comparisons=[]
    keys=('instructions','vector_instructions','read_bytes','write_bytes','fetch_bytes','memory_requests',
          'vector_memory_beats','vector_read_bytes','vector_write_bytes','analog_commands','analog_read_bytes',
          'analog_write_bytes','instruction_bytes','icache_hits','icache_misses','icache_fills')
    for vlen,depth in sorted({(v,d) for v,m,d,b in variants if b == 256}):
        names=[f'{model}-v{vlen}-d{depth}-budget256' for model in ('baseline','current')]
        if not all(n in byname for n in names): continue
        before,after=[byname[n] for n in names]
        assert before['memory_sha256'] == after['memory_sha256'], ('Before/after architectural memory differs',names)
        assert all(before['cpu'][k] == after['cpu'][k] for k in keys+('lsq_enqueued','lsq_completed')), ('Work performed changed',names)
        if depth == 1:
            assert before['trace_hashes'] == after['trace_hashes'], 'Blocking depth-1 trace changed'
            assert before['phases'] == after['phases'], 'Blocking depth-1 timing changed'
        comparisons.append(dict(vlen=vlen,depth=depth,identical_final_memory=True,identical_work=True,
                    baseline_cycles=before['cpu']['end_cycle'],current_cycles=after['cpu']['end_cycle']))
    # Controls are mandatory even when narrowing --vlen or validating archives.
    reference=byname['current-v256-d16-budget256']
    blocking=byname['current-v256-d1-budget256']
    assert blocking['memory_sha256'] == reference['memory_sha256'], 'Blocking and queued paths disagree on architectural state'
    assert all(blocking['cpu'][k] == reference['cpu'][k] for k in keys), 'Blocking and queued paths perform different work'
    replay=byname['current-v256-d16-budget1']
    assert replay['canonical_trace_hashes'] == reference['canonical_trace_hashes'], 'Host instruction budget changed modeled trace'
    assert replay['phases'] == reference['phases'], 'Host instruction budget changed phase timing'
    assert replay['memory_sha256'] == reference['memory_sha256'], 'Host instruction budget changed memory'
    assert replay['array_stats'] == reference['array_stats'], 'Host instruction budget changed array statistics'
    for key in reference['cpu'].keys()-{'grants','stops'}:
        assert replay['cpu'][key] == reference['cpu'][key], ('Host instruction budget changed modeled CPU counters',key)
    dump(output/'validation.json',dict(passed=True,cases=len(results),comparisons=comparisons,
        blocking_and_queued_final_memory_identical=True,
        budget_1_and_256_modeled_trace_identical=True,
        budget_comparison_scope='All analog event fields and order identical after one bijective command-token renumbering; all other CSVs byte-identical; phase and total cycles and final SPM identical. Host grants/stops may differ.',
        validator_sha256=digest(Path(__file__))))
    print(f'PASS {len(results)} analog-register dependency regressions',flush=True)


if __name__ == '__main__': main()
