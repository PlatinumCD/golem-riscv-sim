"""Fast negative and positive checks; no simulator or compiler is launched."""
from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from analyze import task_intervals, workload_observations, valid_receive_order
from cases import registry
from contract import (MeasurementError, csv_rows, metric, required, optional,
                      elapsed, union_duration, rate, save, summary, write_csv)
from predict import features, validate_prediction
from common_records import empty_record, flatten_record, validate_record
from sst_metrics import RequiredMetrics
from run import ROOT, output_directory


class OutputTests(unittest.TestCase):
    def test_default_results_owner(self):
        output = output_directory(None, {'GOLEM_BUILD_ROOT': '/unused'}, {})
        self.assertEqual(output.parent, ROOT / 'tests/results/communication-envelope')

    def test_runner_owner_and_explicit_output(self):
        with tempfile.TemporaryDirectory() as directory:
            owner = Path(directory).resolve()
            paths = {'GOLEM_BUILD_ROOT': str(owner)}
            env = dict(paths, GOLEM_TEST_CASE_NAME='network/communication-envelope')
            self.assertEqual(output_directory(None, paths, env).parent,
                             owner / 'communication-envelope')
            self.assertEqual(output_directory(owner / 'selected', paths, env),
                             owner / 'selected')
            with self.assertRaises(ValueError):
                output_directory(owner.parent / 'escaped', paths, env)
            (owner / 'escape').symlink_to(owner.parent, target_is_directory=True)
            with self.assertRaises(ValueError):
                output_directory(owner / 'escape/result', paths, env)


class MeasurementTests(unittest.TestCase):
    def test_streaming_rx_can_start_before_full_frame(self):
        self.assertTrue(valid_receive_order(100,10,20,20,110,110,120))
        self.assertTrue(valid_receive_order(5,10,20,20,110,110,120))
        self.assertFalse(valid_receive_order(111,10,20,20,110,110,120))
        self.assertFalse(valid_receive_order(100,21,20,20,110,110,120))
        self.assertFalse(valid_receive_order(100,10,20,20,110,109,120))

    def test_missing_is_not_zero(self):
        self.assertIsNone(optional({}, 'missing'))
        for values in ({}, {'missing': None}):
            with self.assertRaises(MeasurementError):
                required(values, 'missing')
        self.assertEqual(required({'zero': 0}, 'zero'), 0)
        value = metric(None, 'cycle', 'resource_sum', 'RX scheduler', reason='no dedicated counter')
        self.assertEqual(value['status'], 'not_measured')
        self.assertIsNone(value['value'])
        with self.assertRaises(MeasurementError):
            metric(None, 'cycle', 'resource_sum', 'RX')
        for value in (-1, True, float('inf'), float('nan')):
            with self.assertRaises(MeasurementError):
                metric(value, 'cycle', 'resource_sum', 'RX')

    def test_resource_overlap_is_not_elapsed_sum(self):
        self.assertEqual(union_duration([(0, 10), (5, 15), (7, 8)]), 15)
        self.assertEqual(elapsed(0, 15), 15)
        self.assertNotEqual(sum(b-a for a,b in [(0,10),(5,15),(7,8)]), 15)
        with self.assertRaises(MeasurementError):
            union_duration([(4, 2)])
        self.assertIsNone(rate(32, 0))
        self.assertIsNone(rate(None, 10))

    def test_serialization_preserves_availability(self):
        with tempfile.TemporaryDirectory(prefix='golem-envelope-contract-') as directory:
            root = Path(directory)
            write_csv(root/'out.csv', [dict(missing=None, observed_zero=0)])
            self.assertEqual(csv_rows(root/'out.csv'), [{'missing':'','observed_zero':'0'}])
            save(root/'summary.json', dict(metrics=[dict(name='x',value=0,status='not_measured')]))
            _, values = summary(root/'summary.json')
            self.assertIsNone(values['x'])
            with self.assertRaises(MeasurementError):
                csv_rows(root/'missing.csv')
            self.assertIsNone(csv_rows(root/'missing.csv', required_file=False))
            save(root/'duplicate.json', dict(metrics=[dict(name='x',value=0,status='available')]*2))
            with self.assertRaises(MeasurementError):
                summary(root/'duplicate.json')

    def test_old_analyzer_fallback_does_not_invent_evidence(self):
        with self.assertRaises(ValueError):
            RequiredMetrics().get('rx_stalls', 0)
        with self.assertRaises(ValueError):
            RequiredMetrics({'rx_stalls': None}).get('rx_stalls', 0)
        record = empty_record('H8', 'rx')
        with self.assertRaises(ValueError):
            validate_record(record)  # Required elapsed time has not been measured.
        record.update(makespan_cycles=1,payload_rate_B_per_cycle=0.0)
        self.assertIsNone(record['rx']['serialization_stall_cycles'])
        self.assertIsNone(flatten_record(record)['rx_serialization_stall_cycles'])
        record['rx']['serialization_stall_cycles'] = -1
        with self.assertRaises(ValueError):
            validate_record(record)

    def test_task_boundaries_are_required(self):
        start = dict(tile_id='0',task_id='100',execution_id='0',event='start',sim_time_ticks='10')
        stop = dict(start,event='finish',sim_time_ticks='20')
        self.assertEqual(task_intervals([start,stop])[(0,100,0)], (10,20))
        for bad in ([start], [stop], [start,start], [dict(start,event='mystery')],
                    [start,dict(stop,sim_time_ticks='5')]):
            with self.assertRaises(MeasurementError):
                task_intervals(bad)

    def test_workload_drain_is_not_fifo_occupancy(self):
        rows = [dict(flow=0,offered_tick=0,enqueue_finish_tick=1000,consumer_tick=10000,payload_bytes=32),
                dict(flow=1,offered_tick=5000,enqueue_finish_tick=6000,consumer_tick=15000,payload_bytes=32)]
        result = workload_observations(rows)
        self.assertEqual(result['max_outstanding_messages'], 2)
        self.assertEqual(result['outstanding_at_end'], 0)
        self.assertEqual(result['drain_after_last_enqueue_cycles'], 9)
        self.assertIsNone(result['queue_depth'])


class ExperimentalDesignTests(unittest.TestCase):
    def test_client_subsets_keep_enabled_client_work_fixed(self):
        cases = registry()
        for mask in range(1,8):
            c = cases[f'clients-{mask}']
            self.assertEqual(c.rvv_iterations, 8192 if mask & 1 else 0)
            self.assertEqual(sum(s==12 for s,d in c.flows), 2 if mask & 2 else 0)
            self.assertEqual(sum(d==12 for s,d in c.flows), 2 if mask & 4 else 0)
            self.assertEqual(c.waves, 8)

    def test_shared_distance_uses_real_distinct_sources(self):
        for c in registry().values():
            if c.study != 'distance-contention':
                continue
            paths = c.document()['paths']
            self.assertEqual(len({s for s,d in c.flows}), len(paths))
            self.assertEqual(len({d for s,d in c.flows}), len(paths))
            self.assertEqual(len({len(p) for p in paths}), 1)
            self.assertTrue(set.intersection(*(set(p) for p in paths)))

    def test_controls_match_payload_and_preserve_directed_links(self):
        cases = registry()
        a,b = (cases[n].document() for n in ('independent-eight-t2','many-to-many-t2'))
        self.assertEqual(a['expected_payload_bytes'], b['expected_payload_bytes'])
        self.assertEqual([len(p) for p in a['paths']], [len(p) for p in b['paths']])
        paths = cases['duplex-t2'].document()['paths']
        self.assertFalse(set(paths[0]) & set(paths[1]))
        shared, disjoint = (cases[n].document()['paths'] for n in ('paths-shared-t2','paths-disjoint-t2'))
        self.assertTrue(set(shared[0]) & set(shared[1]))
        self.assertFalse(set(disjoint[0]) & set(disjoint[1]))

    def test_64k_is_one_case_with_four_descriptors(self):
        c = registry()['payload-65536'].document()
        self.assertEqual((c['segments'],c['waves'],c['expected_payload_bytes']), (4,1,65536))
        self.assertEqual(registry()['payload-16388'].document()['segments'], 2)

    def test_fit_does_not_see_holdout_measurements(self):
        cases = registry()
        records = []
        for c in cases.values():
            if c.fit_role == 'train':
                doc = c.document()
                records.append(dict(case=doc,makespan_cycles=100+256*c.payload/1024+4*len(doc['paths'][0])))
        held = dict(case=cases['source-shared-t2'].document(),makespan_cycles=5000)
        first = validate_prediction(records+[held])
        second = validate_prediction(records+[dict(held,makespan_cycles=10000)])
        self.assertEqual(first['coefficients'], second['coefficients'])
        self.assertEqual(first['predictions'][0]['predicted_cycles'], second['predictions'][0]['predicted_cycles'])
        self.assertNotEqual(first['maximum_absolute_percentage_error'], second['maximum_absolute_percentage_error'])
        self.assertNotIn(held['case']['name'], first['training_cases'])
        self.assertEqual(validate_prediction([])['status'], 'NOT_EVALUATED')
        directions = deepcopy(held['case'])
        labels,_ = features(directions)
        self.assertTrue(any(label.startswith('TX') for label,_,_ in labels))


if __name__ == '__main__':
    unittest.main()
