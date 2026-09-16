"""Verify the diagnostic checklist against saved simulations and attribution."""
import collections
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
RESULTS=ROOT/'tests/results/dma-diagnostics'
RUNS={'baseline':'1789534804566675109','corrected':'1789535332327804596',
      'compute':'1789535365266321361','event':'1789535513412064306',
      'attribution':'1789535871936531791','frequency_attribution':'1789535838786195925',
      'compute_attribution':'1789535825372815405','host':'1789535936111717223',
      'router':'1789536519069916138'}
EXPECTED={'baseline':60,'corrected':60,'compute':30,'event':72,
          'attribution':6,'frequency_attribution':8,'compute_attribution':10,'host':18,'router':6}

def read(path): return json.loads(path.read_text())
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    studies={}; hardware=None; traces=0; handling_count=0
    for name,run in RUNS.items():
        root=RESULTS/run
        complete=read(root/'complete.json'); results=read(root/'results.json')
        assert complete==dict(status='PASS',cases=EXPECTED[name])
        assert len(results)==len(read(root/'plan.json'))==EXPECTED[name]
        provenance=read(root/'provenance.json')
        assert all(digest(Path(p))==h for p,h in provenance['binaries'].items()),name
        assert (root/'ram.bin').read_bytes()==bytes(range(256))*256
        for r in results:
            assert r['status']=='PASS' and r['host_wall_seconds']>0
            assert r['whole_run_bridge_events'] is not None and r['whole_run_grants'] is not None
            for elf,h in r['guest_sha256'].items(): assert digest(Path(r['guest'])/elf)==h
            trial=Path(r['directory']); timeline=read(trial/'timeline.json')
            assert int(timeline['dma']['byte_count'])==65536 and r['ram_service_cycles']==18440
            config=read(next((trial/'profile/resolved').glob('tile-0-*.json')))['parameters']
            selected={k:v['value'] for k,v in config.items() if v['category']=='hardware'}
            if hardware is None: hardware=selected
            assert selected==hardware,(name,r['directory'])
            assert int(config['sync_instruction_quantum']['value'])==r['quantum']
            if 'attribution' in name:
                attribution=read(trial/'attribution.json'); traces+=1
                assert attribution['attributed_retired']==attribution['total_retired']==r['instructions']
                assert sum(attribution['categories'].values())==r['instructions']
                if r.get('work_iterations',0):
                    assert attribution['categories']['useful_register_add']==8*r['work_iterations']
                if r['rounds']:
                    assert len(timeline['software_handling'])==64
                    for request,handle,reply,receipt in zip(timeline['requests'],timeline['software_handling'],timeline['replies'],timeline['peer_received']):
                        assert int(request['event_tick'])<=int(handle['sim_time_ticks'])<=int(reply['event_tick'])<=int(receipt['event_tick'])
                        handling_count+=1
            assert len(timeline['requests'])==len(timeline['replies'])==len(timeline['peer_received'])==r['rounds']
            if not r['rounds']:
                assert r['first_reply_issue_cycles'] is None and r['round_trips_per_cycle'] is None
        studies[name]=results
    for before,after in zip(studies['baseline'],studies['corrected']):
        for field in ('rounds','interval','mode','quantum','repeat','guest_sha256'):
            assert before[field]==after[field],field
    # Quantum changes must no longer change simulated timing or packet order.
    for cohort in ('corrected','event'):
        groups=collections.defaultdict(list)
        for r in studies[cohort]:
            if r['interval']==1: groups[(r['rounds'],r['mode'])].append(r)
        for key,rs in groups.items():
            assert len(rs)==12
            assert len({r['elapsed_cycles'] for r in rs})==1
            assert len({r['instructions'] for r in rs})==1
            signatures=[]
            for r in rs:
                t=read(Path(r['directory'])/'timeline.json'); start=t['cpu_region'][0]
                signatures.append(tuple(tuple((row['event'],int(row['packet_id']),int(row['event_tick'])-start)
                    for row in t[k]) for k in ('requests','replies','peer_received')))
            assert all(x==signatures[0] for x in signatures)
    assert {r['interval'] for r in studies['frequency_attribution'] if r['mode']==1 and r['rounds']==64}=={1,4,16,64,256}
    assert {r['work_iterations'] for r in studies['compute']}=={0,128,512,2048,8192}
    router_audit=read(RESULTS/RUNS['router']/'router-audit.json')
    assert router_audit==dict(status='PASS',cases=6,verified_router_services=768)
    for traced in studies['router']:
        reference=next(r for r in studies['host'] if (r['rounds'],r['mode'])==(traced['rounds'],traced['mode']))
        for field in ('elapsed_cycles','instructions','guest_sha256'):
            assert traced[field]==reference[field],field
        resources=read(Path(traced['directory'])/'resource-intervals.json')
        assert all(o['queue_cycles']==0 for o in resources['network_outputs'].values())
    report=dict(status='PASS',runs=RUNS,validated_simulations=sum(EXPECTED.values()),
                attributed_regions=traces,verified_request_handling_chains=handling_count,
                verified_router_services=router_audit['verified_router_services'],
                hardware_parameters=hardware,input_sha256=digest(RESULTS/RUNS['baseline']/'ram.bin'),
                proof_scope='Synthetic two-tile DMA/mesh diagnostics only; no full-model performance claim.')
    path=RESULTS/'report/checklist-audit.json'
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('hardware_parameters','runs')},indent=2))

if __name__=='__main__': main()
