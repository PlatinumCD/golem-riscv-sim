"""Guest-initiated whole-message transfers, ownership, backpressure and CPU waits."""
import argparse
import csv
import hashlib
import json
import os
import re
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
from build import build, load_build_info

TILE_PROFILES = json.loads((SOURCE / "tile_profiles.json").read_text())
SPM_BASE = 0x90000000
RX_A = SPM_BASE + 0x60000
RX_B = SPM_BASE + 0x80000
OUTPUT_OFFSET = 0xa0000
REPORT_OFFSET = 0xe0000


def cases():
    return [dict(name=f'bytes-{size}', size=size, kind=0) for size in (64, 256, 4096, 65536)] + [
        dict(name='independent-transfers-same-destination', size=256, kind=1, slots=1, mesh_y=2, destination=3),
        dict(name='multiple-senders-reserved-inputs', size=256, kind=8, slots=1, mesh_y=2, destination=3),
        dict(name='completed-ticket-full', size=64, kind=2, slots=1, tickets=1, commands=1),
        dict(name='active-command-full', size=65536, kind=3, slots=1, commands=1),
        dict(name='budget-1', size=256, kind=0, budget=1),
        dict(name='multi-vc', size=4096, kind=0, vcs=4),
        dict(name='profile-on', size=256, kind=0, profile=True),
        dict(name='packet-16', size=256, kind=0, packet=16),
        dict(name='lsq-1', size=256, kind=0, depth=1),
        dict(name='single-packet-slot', size=4096, kind=0, window=1, fragments=1, packet_slots=1, credit_delay=16),
        dict(name='shared-bank-permissions', size=64, kind=0, shared_banks=True, slots=1, packet=33),
        dict(name='exact-nic-capacity', size=256, kind=0, nic_bytes=352),
        dict(name='wide-flit-minimum-packet', size=256, kind=0, flit_bits=512, packet=16, nic_bytes=128),
        # Keep source completion observably ahead of destination commit even
        # when LLVM changes the sender's descriptor/bookkeeping instruction count.
        dict(name='ordered-completion', size=65536, kind=5, link_latency='128ns'),
        dict(name='ordered-single-slot', size=65536, kind=5, slots=1),
        dict(name='reverse-destination', size=256, kind=0, sender=1, destination=0),
        dict(name='vertical-destination', size=256, kind=0, mesh_x=1, mesh_y=2),
        dict(name='multi-hop-xy', size=4096, kind=0, mesh_y=2, destination=3),
        dict(name='multi-hop-xy-reverse', size=4096, kind=0, mesh_y=2, sender=3, destination=0),
        dict(name='bidirectional', size=256, kind=6),
        dict(name='id-zero', size=64, kind=0, transfer_a=0, invocation_first=0),
        dict(name='id-63-bit-limit', size=64, kind=0, transfer_a=(1<<63)-1, invocation_first=(1<<63)-2),
        dict(name='source-reuse-before-consumption', size=4096, kind=0, reuse=True, delay=20000),
        dict(name='delayed-credit-return', size=64, kind=0, mesh_y=2, destination=3, link_latency='128ns'),
        dict(name='try-receive-ready', size=64, kind=0, try_receive=True, delay=2000),
        dict(name='reserved-encodings', size=64, kind=7),
        dict(name='mvm-multi-hop-mvm', size=128, kind=4, mesh_y=2, destination=3)] + [
        dict(name=f'profile-{profile}-{name}', profile_name=profile, size=size, kind=kind,
             mesh_y=2, destination=3, **extra)
        for profile in TILE_PROFILES
        for name, size, kind, extra in (
            ('message', 4096, 0, dict(reuse=True, delay=20000)),
            ('reserved-inputs', 256, 8, dict(slots=1)),
            ('mvm', 128, 4, dict(background_array=False)))
    ]


def deployment(case):
    a=case.get('transfer_a',0x123456789abcdef)
    b=0x23456789abcdef0
    first=case.get('invocation_first',0x3456789abcdef01)
    source, destination=case.get('sender',0), case.get('destination',1)
    slots=case.get('slots',2)
    def record(identity, producer, consumer, base):
        return dict(transfer_id=identity,source_tile=producer,destination_tile=consumer,
                    receive_base=base,slot_capacity=case['size'],slot_count=slots)
    transfers=[] if case['kind']==7 else [record(a,source,destination,RX_A)]
    if case['kind'] in (1,8): transfers.append(record(b,1 if case['kind']==8 else source,destination,RX_B))
    if case['kind']==6: transfers.append(record(b,destination,source,RX_B))
    return dict(network_transfers=transfers,transfer_a=a,transfer_b=b,invocation_first=first,
                invocation_second=first+1,sender=source,destination=destination)


def configure(case, qemu):
    configured = case|deployment(case)|dict(parameters=dict(spm_capacity_bytes=2*1024*1024,spm_banks=4,cpu_spm_banks=[0,1,2,3],
                       router_spm_banks=[0,1] if case.get('shared_banks') else [0,1,2,3],
                       spm_bank_width=32 if case.get('shared_banks') else 4,
                       array_rows=32,array_cols=32,arrays_per_tile=2 if case['kind']==4 and case.get('background_array',True) else 1,
                       cost_per_mvm_cycles=50000 if case['kind']==4 and case.get('background_array',True) else 100),
            cpu_parameters=dict(instruction_budget=case.get('budget',256),load_store_queue_depth=case.get('depth',4)),
            router_parameters=dict(request_window=case.get('window',8),max_request_bytes=case.get('packet',256),
                memory_queue_depth=case.get('fragments',8),
                posted_receive_slots_per_source=case.get('packet_slots',4),
                posted_credit_delay_cycles=case.get('credit_delay',4),net_command_queue_depth=case.get('commands',4),
                net_ticket_capacity=case.get('tickets',8)),
            profile=case.get('profile',os.environ.get('TILE_CYCLE_PROFILE')=='1'),qemu=str(qemu.resolve()))
    if 'profile_name' in case:
        profile = TILE_PROFILES[case['profile_name']]
        for group in ('parameters','cpu_parameters','router_parameters'):
            configured[group].update(profile[group])
        configured['mesh_parameters'] = profile['mesh_parameters']
        configured['flit_bits'] = profile['mesh_parameters']['flit_size_bits']
    return configured


def pattern(size, seed):
    return struct.pack('<'+'I'*(size//4), *[((i*37+seed*101)^0x3f123456)&0xffffffff for i in range(size//4)])


def check_encoding(output, compiler):
    directory=output/'encoding'; directory.mkdir()
    compilers=[('llvm',compiler,['--target=riscv64-unknown-elf'])]
    gnu=ROOT/'install/riscv-gnu-toolchain/bin/riscv64-unknown-elf-gcc'
    if gnu.is_file(): compilers.append(('gnu',gnu,[]))
    expected=[0x2b|(10<<7)|(op<<12)|((0 if op==1 else 11)<<15)|(second<<20)|(variant<<25)
        for op,second,variant in [(0,12,0),(1,0,0),(1,0,1),(2,12,0),(3,0,0),(4,0,0),(4,0,1)]]
    checked=[]
    for name,cc,flags in compilers:
        obj=directory/f'{name}.o'; binary=directory/f'{name}.bin'
        subprocess.run([str(cc),*flags,'-march=rv64gcv','-mabi=lp64d','-I',
            str(SOURCE/'components/mordred'),'-c',str(HERE/'encoding.S'),'-o',str(obj)],check=True)
        subprocess.run([str(ROOT/'install/llvm/bin/llvm-objcopy'),'-O','binary','-j','.text',
            str(obj),str(binary)],check=True)
        assert struct.unpack('<7I',binary.read_bytes())==tuple(expected), name
        checked.append(name)
    (directory/'validation.json').write_text(json.dumps(dict(passed=True,compilers=checked,instructions=7),indent=2)+'\n')


def compile_guests(trial, case, compiler):
    capacity=case['parameters']['spm_capacity_bytes']
    vlen=case['parameters'].get('riscv_vector_length_bits',256)
    assert capacity >= 1024*1024, 'network fixture requires at least 1 MiB'
    (trial/'layout.h').write_text(
        f'#define RX_A UINT64_C({RX_A})\n#define RX_B UINT64_C({RX_B})\n'
        f'#define OUTPUT UINT64_C({SPM_BASE+OUTPUT_OFFSET})\n'
        f'#define REPORT_ADDRESS {SPM_BASE+REPORT_OFFSET}\n'
        f'#define REPORT ((volatile uint64_t*){SPM_BASE+REPORT_OFFSET}UL)\n'
        f'#define SPM_LAST_WORD ((volatile uint32_t*){SPM_BASE+capacity-4}UL)\n')
    lines=[]
    for name, seed in (('a', 1), ('b', 2), ('c', 3)):
        payload=(struct.pack('<32f',*[float(i+seed) for i in range(32)]) if case['kind']==4
                 else pattern(case['size'], seed))
        values=struct.unpack('<'+'I'*(case['size']//4), payload)
        lines.append(f'__attribute__((section(".source_{name}"), aligned(64)))\n'
                     f'uint32_t source_{name}[{len(values)}] = {{'+','.join(hex(v) for v in values)+'};\n')
    (trial/'payload.h').write_text(''.join(lines))
    (trial/'deployment.h').write_text('\n'.join(
        f'#define {key.upper()} UINT64_C({case[key]})'
        for key in ('transfer_a','transfer_b','invocation_first','invocation_second'))+
        '\n#define UNCONFIGURED_TRANSFER UINT64_C(99)\n')
    elfs=[]
    width, height=case.get('mesh_x',2),case.get('mesh_y',1)
    for tile in range(width*height):
        elf=trial/f'tile{tile}.elf'
        command=[str(compiler), '--target=riscv64-unknown-elf', '-mcpu=golem-analog',
                 f'-march=rv64gcv_zvl{vlen}b_xgolemanalog', f'-mrvv-vector-bits={vlen}',
                 '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0', '-O2',
                 '-fno-vectorize', '-fno-slp-vectorize', '-ffreestanding', '-fno-builtin',
                 '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,--no-relax', '-Wl,--build-id=none',
                 f'-I{trial}', f'-DTILE={tile}', f'-DMESSAGE_BYTES={case["size"]}',
                 f'-DCASE={case["kind"]}', f'-DBACKGROUND_ARRAY={int(case.get("background_array",True))}',
                 f'-Wl,--defsym=SPM_BYTES={capacity}', f'-DRECEIVE_SLOTS={case.get("slots", 2)}',
                 f'-DSENDER={case["sender"]}', f'-DDESTINATION={case["destination"]}',
                 f'-DCHECK_BANKS={int(case.get("shared_banks",False))}', f'-DTILE_COUNT={width*height}', f'-DREUSE_SOURCE={int(case.get("reuse",False))}',
                 f'-DTRY_RECEIVE={int(case.get("try_receive",False))}',
                 f'-DDELAY={case.get("delay", 0)}', '-T', str(HERE/'scratchpad.ld'),
                 str(HERE.parent/'riscv-qemu/start.S'),
                 str(HERE/{4:'mvm_guest.c',7:'retired_setup.c'}.get(case['kind'],'guest.c')), '-o', str(elf)]
        result=subprocess.run(command, capture_output=True, text=True, timeout=90)
        (trial/f'compile{tile}.log').write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
        elfs.append(str(elf))
    return elfs


def rows(path):
    with path.open() as stream:
        return [{k: v if k=='event' else int(v) for k,v in r.items()} for r in csv.DictReader(stream)]


def reports(text, label):
    # Validation indexes these records by physical tile ID. Lexical ordering
    # puts tile10 before tile2 and is incorrect for meshes larger than 3x3.
    return sorted([json.loads(line[len(label)+1:]) for line in text.splitlines()
                   if line.startswith(label+' ')],
                  key=lambda r: int(re.search(r'(?:^|\.)tile(\d+)\.', r['component']).group(1)))


def validate(trial, case):
    log=(trial/'simulation.log').read_text()
    cpu=reports(log, 'RISCV_STATS'); net=reports(log, 'NETWORK_STATS'); spm=reports(log, 'MORDRED_SPM_STATS')
    tiles=case.get('mesh_x',2)*case.get('mesh_y',1)
    capacity=case['parameters']['spm_capacity_bytes']
    for tile in range(tiles):
        backing=trial/f'tile{tile}-spm.bin'
        assert backing.stat().st_size==capacity, 'SPM backing must match the chosen capacity'
        with backing.open('rb') as stream:
            stream.seek(capacity-4)
            assert stream.read(4)==struct.pack('<I',0x53504d00+tile), 'last SPM word inaccessible'
    assert len(cpu)==len(net)==len(spm)==tiles, log[-4000:]
    if case['kind']==7:
        for tile in range(tiles):
            data=(trial/f'tile{tile}-spm.bin').read_bytes()
            assert struct.unpack_from('<Q',data,REPORT_OFFSET)==(0x4e45544f4b,)
            assert struct.unpack_from('<Q',data,REPORT_OFFSET+24)==(2,), 'reserved encodings must trap'
            assert cpu[tile]['network_commands']==net[tile]['submitted']==net[tile]['received']==0
            assert spm[tile]['bytes_read']==spm[tile]['bytes_written']==net[tile]['receive_slots']==0
            assert net[tile]['idle'] and spm[tile]['idle']
        return dict(passed=True,cpu=cpu,network=net,spm=spm,illegal_instruction_cause=2,reserved_encodings=3)
    source,destination=case['sender'],case['destination']
    a,b=case['transfer_a'],case['transfer_b']
    first,second=case['invocation_first'],case['invocation_second']
    size,kind=case['size'],case['kind']
    # sender, receiver, transfer, invocation, hardware sequence, bytes, output, pattern
    deliveries=[(source,destination,a,first,1,size,0,1)]
    if kind==6: deliveries.append((destination,source,b,first,1,size,0,2))
    elif kind==8:
        deliveries += [(1,destination,b,first,1,size,1,2),(1,destination,b,second,2,size,2,3)]
    elif kind!=4:
        deliveries.append((source,destination,a,second,2,64 if kind==5 else size,1,2))
        if kind==1: deliveries.append((source,destination,b,first,1,size,2,3))
    messages=[rows(trial/f'net.tile{tile}.router_spm-messages.csv') for tile in range(tiles)]
    calls=[rows(trial/f'net.tile{tile}.riscv-network.csv') for tile in range(tiles)]
    for tile in range(tiles):
        sent=[d for d in deliveries if d[0]==tile]
        received=[d for d in deliveries if d[1]==tile]
        assert net[tile]['submitted']==net[tile]['source_complete']==len(sent), (tile,net[tile],sent)
        assert net[tile]['received']==net[tile]['released']==len(received)
        assert net[tile]['payload_bytes']==sum(d[5] for d in sent)
        assert spm[tile]['bytes_read']==net[tile]['payload_bytes']+net[tile]['descriptor_bytes']
        assert spm[tile]['bytes_written']==sum(d[5] for d in received)
        if not sent: assert net[tile]['descriptor_bytes']==0
        active=[t for t in case['network_transfers'] if tile in (t['source_tile'],t['destination_tile'])]
        slots=sum(t['slot_count'] for t in active if t['destination_tile']==tile)
        assert net[tile]['configured_transfers']==len(active) and net[tile]['receive_slots']==slots
        if not active:
            assert cpu[tile]['network_commands']==0, 'intermediate CPU participated in forwarding'
            assert spm[tile]['bytes_read']==spm[tile]['bytes_written']==0, 'intermediate SPM participated'
        data=(trial/f'tile{tile}-spm.bin').read_bytes()
        report=struct.unpack_from('<8Q',data,REPORT_OFFSET)
        assert report[:3]==(0x4e45544f4b,kind,size), report
        if tile==destination and kind==4:
            assert struct.unpack_from('<32f',data,OUTPUT_OFFSET)==tuple(float(2*(i+1)) for i in range(32))
        else:
            for _,_,_,_,_,length,output,seed in received:
                assert data[OUTPUT_OFFSET+output*65536:OUTPUT_OFFSET+output*65536+length]==pattern(length,seed)
        if kind==8 and tile==destination:
            assert data[OUTPUT_OFFSET+3*65536:OUTPUT_OFFSET+3*65536+size]==pattern(size,2), 'held buffer changed before release'
        assert len({report[4+d[6]] for d in received})==len(received), 'receive token alias'
        assert net[tile]['idle'] and spm[tile]['idle']
        # Only message data and the two hardware credit classes cross the mesh.
        assert 'responses_sent' not in spm[tile] and 'responses_received' not in spm[tile]
        assert spm[tile]['posted_accepted']==spm[tile]['requests_sent']==spm[tile]['credits_received']
        assert spm[tile]['posted_committed']==spm[tile]['requests_received']==spm[tile]['credits_sent']
        assert net[tile]['max_commands']<=case['router_parameters']['net_command_queue_depth']
        assert net[tile]['max_tickets']<=case['router_parameters']['net_ticket_capacity']
        assert net[tile]['max_packet_buffers']<=case['router_parameters']['request_window']
        assert net[tile]['max_occupied_slots']<=slots
        assert spm[tile]['max_local_requests']<=case['router_parameters']['request_window']+1
        assert spm[tile]['max_pending_memory']<=case['router_parameters']['memory_queue_depth']
        assert spm[tile]['max_posted_receive_reservations']<=case['router_parameters']['posted_receive_slots_per_source']
        assert spm[tile]['packets_injected']==(spm[tile]['requests_sent']+
                spm[tile]['credit_packets_sent']+net[tile]['control_packets_sent'])
        assert len([r for r in messages[tile] if r['event']=='retire'])==len(sent)
        assert net[tile]['control_packets_sent']==len(received), 'only releases return application credits'
        assert all(r['operation'] in (0,1,2,3,4,257,260) for r in calls[tile])
        assert all(r['first']==r['second']==0 for r in calls[tile] if r['operation'] in (1,257))
        acquired=[r for r in messages[tile] if r['event']=='acquire']
        assert sorted((r['transfer_id'],r['invocation_id'],r['bytes']) for r in acquired)==sorted(
                (d[2],d[3],d[5]) for d in received)
        for transfer in {d[2] for d in received}:
            assert [r['invocation_id'] for r in acquired if r['transfer_id']==transfer]==[
                d[3] for d in sorted(received,key=lambda d:d[4]) if d[2]==transfer]
        for ready in (r for r in messages[tile] if r['event']=='ready'):
            chunks=[r for r in messages[tile] if r['event']=='chunk_commit' and
                    (r['transfer_id'],r['identity'],r['invocation_id'])==
                    (ready['transfer_id'],ready['identity'],ready['invocation_id'])]
            assert sum(r['bytes'] for r in chunks)==ready['bytes']
            assert ready['cycle']==max(r['cycle'] for r in chunks)
        for token in acquired:
            release=next(r for r in messages[tile] if r['event']=='release' and r['identity']==token['identity'])
            assert release['cycle']>token['cycle']
    assert sum(p['wire_bytes_sent'] for p in spm)==sum(p['wire_bytes_received'] for p in spm)
    assert sum(p['control_packets_sent'] for p in net)==sum(p['control_packets_received'] for p in net)
    if case.get('link_latency'):
        final_credit=max(r['cycle'] for c in messages for r in c if r['event']=='slot_credit_received')
        assert final_credit>max(c['end_cycle'] for c in cpu), 'exercise credit return after all CPUs exit'
    route_checks=[]
    if kind in (0,4,5) and tiles>2:
        # Reconcile actual Mordred output flits against both directed XY paths.
        with (trial/'network-statistics.csv').open() as stream:
            stats=[{key.strip():value.strip() for key,value in row.items()} for row in csv.DictReader(stream)]
        width=case.get('mesh_x',2)
        expected={}
        for origin,target in ((source,destination),(destination,source)):
            tile=origin
            while tile!=target:
                x,y=tile%width,tile//width; tx,ty=target%width,target//width
                port=1 if x<tx else 3 if x>tx else 0 if y<ty else 2
                expected[tile,port]=expected.get((tile,port),0)+spm[origin]['wire_bytes_sent']
                tile += {0:width,1:1,2:-width,3:-1}[port]
            expected[tile,4]=expected.get((tile,4),0)+spm[origin]['wire_bytes_sent']
        for tile in range(tiles):
            for port in range(5):
                matching=[r for r in stats if r['ComponentName']==f'net.router.{tile%width}.{tile//width}' and
                    r['StatisticName']=='sent_flit_cnt' and r['StatisticSubId'].startswith(f'{tile}_{port}_')]
                actual=sum(int(r['Sum.u64']) for r in matching)*(case.get('flit_bits',128)//8)
                assert actual==expected.get((tile,port),0), (tile,port,actual,expected)
                if actual: route_checks.append(dict(tile=tile,port=port,wire_bytes=actual))
    if kind==4:
        arrays=reports(log,'ARRAY_STATS')
        assert len(arrays)==tiles and [r['mvms'] for r in arrays]==[case['parameters']['arrays_per_tile'] if i in (source,destination) else 0 for i in range(tiles)]
        if case.get('background_array',True):
            events=rows(trial/f'net.tile{source}.arrays.csv')
            starts=[r for r in events if r['array']==1 and r['operation']==2 and r['event']=='start']
            ends=[r for r in events if r['array']==1 and r['operation']==2 and r['event']=='complete']
            wait=next(r for r in calls[source] if r['event']=='complete' and r['operation']==4)
            assert len(starts)==len(ends)==1 and starts[0]['cycle']<wait['cycle']<ends[0]['cycle']
            with (trial/f'net.tile{source}.riscv-waits.csv').open() as stream:
                waits=list(csv.DictReader(stream))
            assert any(r['stop_reason']=='33' and int(r['end_cycle'])>int(r['start_cycle']) for r in waits)
        assert (trial/f'tile{source}-spm.bin').read_bytes()[0x50000:0x50080]==b'\xff'*128
    if kind in (1,8):
        held=a if kind==1 else b
        other=b if kind==1 else a
        acquired=next(r for r in messages[destination] if r['event']=='acquire' and r['transfer_id']==held)
        release=next(r for r in messages[destination] if r['event']=='release' and r['identity']==acquired['identity'])
        other_ready=next(r for r in messages[destination] if r['event']=='ready' and r['transfer_id']==other)
        assert acquired['cycle']<other_ready['cycle']<release['cycle'], 'reserved input could not progress'
        assert any(r['operation']==260 and r['result']==-6 for c in calls for r in c if r['event']=='complete')
    if kind in (2,3):
        assert net[source]['would_block']>=1
        busy=next(r for r in calls[source] if r['event']=='complete' and r['operation']==0 and r['result']==-1)
        first_complete=next(r for r in messages[source] if r['event']=='source_complete')
        assert (first_complete['cycle']<busy['cycle'])==(kind==2)
    if kind==5:
        ready={r['identity']:r['cycle'] for r in messages[destination] if r['event']=='ready'}
        acquired=[r for r in messages[destination] if r['event']=='acquire']
        assert [r['bytes'] for r in acquired]==[65536,64]
        assert acquired[0]['cycle']>=ready[1] and acquired[1]['cycle']>=ready[2]
        if case.get('slots',2)==2:
            assert ready[2]<ready[1], 'later short message must finish first but be acquired second'
            retired=[r for r in messages[source] if r['event']=='retire']
            complete={r['identity']:r['cycle'] for r in messages[source] if r['event']=='source_complete'}
            assert retired[0]['cycle']<complete[retired[1]['identity']], 'target wait drained unrelated send'
            assert retired[0]['cycle']<ready[2], 'fixture must separate source capture from remote commit'
    reuse_check=None
    if case.get('reuse'):
        # The fixture first waits on the invalid ticket 42. Correlate the actual
        # send's positive ticket with its successful wait, not that early error.
        send=next(r for r in calls[source] if r['event']=='complete' and
                  r['operation']==0 and r['result']>0)
        wait=next(r for r in calls[source] if r['event']=='complete' and
                  r['operation']==4 and r['first']==send['result'] and r['result']==0)
        capture=next(r for r in messages[source] if r['event']=='source_complete' and
                     r['identity']==send['result'])
        acquire=next(r for r in messages[destination] if r['event']=='acquire' and
                     r['transfer_id']==a and r['invocation_id']==first)
        memory=rows(trial/f'net.tile{source}.riscv-memory.csv')
        base=0x90020000
        writes=[r for r in memory if r['write'] and base<=r['address']<base+size]
        issued=[r for r in writes if r['event']=='issue']
        completed=[r for r in writes if r['event']=='ready']
        assert issued and completed, 'source overwrite must have timed CPU stores'
        cursor=base
        for write in sorted(completed,key=lambda r:r['address']):
            assert write['address']==cursor and write['bytes']>0, 'overwrite must cover every source byte once'
            cursor+=write['bytes']
        assert cursor==base+size
        first_write=min(r['cycle'] for r in issued)
        last_write=max(r['cycle'] for r in completed)
        assert capture['cycle']<=wait['cycle']<first_write<=last_write<acquire['cycle'], \
            'source overwrite must finish after successful wait and before receiver acquisition'
        assert (trial/f'tile{source}-spm.bin').read_bytes()[0x20000:0x20000+size]==b'\xff'*size
        reuse_check=dict(ticket=send['result'],source_complete_cycle=capture['cycle'],
                         wait_cycle=wait['cycle'],overwrite_start_cycle=first_write,
                         overwrite_complete_cycle=last_write,receive_acquire_cycle=acquire['cycle'])
    # Charge the only supported data header on every packet, including partial
    # tails, and check the separate packet and application credit formats.
    for tile in range(tiles):
        events=rows(trial/f'net.tile{tile}.router_spm-spm.csv')
        flit=case.get('flit_bits',128)//8
        def wire(bytes): return max(2,(bytes+flit-1)//flit)*flit
        sent=[r for r in events if r['event']=='request_send']
        assert all(r['write']==1 and r['metadata']==0 for r in sent)
        assert len(sent)==spm[tile]['requests_sent']
        expected=(sum(wire(96+r['bytes']) for r in sent)+
                  wire(16)*spm[tile]['credit_packets_sent']+wire(80)*net[tile]['control_packets_sent'])
        assert spm[tile]['wire_bytes_sent']==expected, (tile,spm[tile]['wire_bytes_sent'],expected)
    if case['profile']: assert list(trial.glob('*-cycles.csv')) or list(trial.glob('*cycle*.jsonl')) or list(trial.glob('*profile*'))
    return dict(passed=True,cpu=cpu,network=net,spm=spm,spm_capacity_bytes=capacity,
                exact_payload_bytes=sum(d[5] for d in deliveries), xy_router_outputs=route_checks,
                compiler_identities_preserved=True,ownership_checked=True,arrival_after_all_spm_writes=True,
                source_reuse=reuse_check,
                message_trace_sha256=[hashlib.sha256(json.dumps([
                    row | ({'identity':0} if row['event'] in ('command','reply') else {})
                    for row in records],sort_keys=True).encode()).hexdigest() for records in messages])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',action='append')
    parser.add_argument('--build-info',type=Path)
    parser.add_argument('--compiler',type=Path,default=ROOT/'install/llvm/bin/clang')
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/src/qemu/qemu-system-riscv64')
    parser.add_argument('--output',type=Path,default=ROOT/'tests/results/network-instructions'/str(time.time_ns()))
    args=parser.parse_args()
    selected=cases()
    if args.case:
        unknown=set(args.case)-{c['name'] for c in selected}
        if unknown: parser.error(f'Unknown cases: {sorted(unknown)}')
        selected=[c for c in selected if c['name'] in args.case]
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    check_encoding(output,args.compiler)
    info=load_build_info(args.build_info) if args.build_info else build(output/'components')
    results=[]
    for case in selected:
        trial=output/case['name']; trial.mkdir()
        case=configure(case,args.qemu)
        case['elfs']=compile_guests(trial,case,args.compiler)
        (trial/'case.json').write_text(json.dumps(case,indent=2)+'\n')
        env=os.environ|dict(SST_LIB_PATH=info['plugin']+':'+info['library'], TILE_COMPONENT_OUTPUT=str(trial),
             TILE_CYCLE_PROFILE='1' if case['profile'] else '0',PYTHONDONTWRITEBYTECODE='1')
        env.pop('TILE_COMPONENT_TRACE_START_TASK',None)
        command=[info['sst'],'--num-threads=1',f'--output-json={trial/"topology.json"}',str(HERE/'simulation.py')]
        start=time.monotonic()
        with (trial/'simulation.log').open('w') as log:
            child=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try: child.wait(timeout=180)
            finally:
                try: os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                child.wait()
        if child.returncode: raise RuntimeError(f'{case["name"]}: '+(trial/'simulation.log').read_text()[-5000:])
        result=dict(case=case['name'],host_seconds=time.monotonic()-start,**validate(trial,case))
        results.append(result)
        (trial/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print('PASS',case['name'],[c['end_cycle'] for c in result['cpu']],flush=True)
    by_name={r['case']:r for r in results}
    if 'bytes-256' in by_name:
        reference=by_name['bytes-256']
        for name in ('budget-1','profile-on'):
            if name in by_name:
                candidate=by_name[name]
                assert candidate['message_trace_sha256']==reference['message_trace_sha256'], name
                for a,b in zip(candidate['cpu'],reference['cpu']):
                    for field in ('end_cycle','instructions','vector_instructions','network_commands'):
                        assert a[field]==b[field], (name,field,a[field],b[field])
    (output/'validation.json').write_text(json.dumps(dict(passed=True,cases=len(results)),indent=2)+'\n')
    print('PASS',output/'validation.json',flush=True)


if __name__=='__main__': main()
