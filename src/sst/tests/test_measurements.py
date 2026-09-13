#!/usr/bin/env python3
"""Positive and corruption tests for the artifact checker; no builds/simulations."""
import copy
import csv
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

import check_measurements as checker

IDENTITY = 'sha256:' + 'a' * 64


def optional(value):
    return {'value': value, 'status': 'available' if value is not None else 'unknown'}


def metric(value=0, status='available', semantics=('event', 'none', 'count')):
    return dict(value=value, status=status, unit=semantics[0], domain=semantics[1],
                aggregation=semantics[2], known_scope='Fixture observation scope, not elapsed time.')


class MeasurementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='measurement-check-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / 'run' / 'profile'
        self.directory.mkdir(parents=True)
        self.path = self.directory / 'tile-7-summary.json'
        self.config_path = self.directory / 'resolved' / 'tile-7.json'
        self.config_path.parent.mkdir()
        self.config = {'schema_version': 1, 'parameters': {
            key: {'value': value, 'category': 'hardware'} for key, value in {
                'tile_id': 7, 'launch_mode': 'managed',
                'analog_array_count': 1, 'network_size': 8,
                'tx_dma_streams': 1, 'profile_mode': 'trace', 'cpu_clock': '500MHz',
                'rx_dma_clock': '2GHz', 'analog_link_clock': '250MHz',
                # An external mesh clock is NOT an observed NIC/router factor.
                'mesh_link_clock': '100MHz',
            }.items()}}
        self.data = {
            'schema': 'mittens.summary', 'schema_version': 1,
            'contract': 'src/sst/profiling/MEASUREMENT_CONTRACT.md',
            'known_scope': 'Actual Tile fixture, not generic legacy-writer evidence.',
            'legacy_csv_policy': 'Unavailable JSON values are null; CSV may retain legacy payload.',
            'metadata': {'source_tree': optional('src'), 'implementation_id': optional(IDENTITY),
                         'build_id': optional(None), 'run_manifest_reference': optional(None),
                         'resolved_config_reference': optional(str(self.config_path))},
            'timebase': {'sst_timebase': optional('1 ps'), 'factor_unit': 'sst_ticks_per_domain_cycle',
                         'domain_factors': {key: optional(value) for key, value in
                                            {'cpu': 2000, 'rx': 500, 'analog': 4000,
                                             'nic': None, 'router': None, 'global_ram': None}.items()}},
            'tx_observations': dict(unit='cycle', domain='cpu', aggregation='observed_state_duration',
                                    known_scope='A shared observation window, not event-complete utilization.',
                                    bucket_labels=[0, 1, 2, 3, '4+'],
                                    observed_cycles=metric(10, semantics=('cycle', 'cpu', 'observation_window_duration'))),
            'metrics': [dict(name=name, **metric(semantics=semantics)) for name, semantics in checker.METRICS.items()],
        }
        self.named('tile_id')['value'] = 7
        self.named('finish_tick')['value'] = 100000
        self.named('instructions')['value'] = 50
        self.named('vector_instructions')['value'] = 4
        self.named('cpu_cycles')['value'] = 30
        self.named('physical_global_dma_submitted')['value'] = 2
        self.named('physical_global_dma_completed')['value'] = 1
        self.named('analog_commands_submitted')['value'] = 3
        self.named('analog_commands_completed')['value'] = 2
        self.named('scratchpad_service_cycles')['value'] = 12
        self.named('scratchpad_read_service_cycles')['value'] = 5
        self.named('scratchpad_write_service_cycles')['value'] = 7
        for name, buckets in (('ready_cycles', [2, 1, 3, 0, 4]),
                              ('active_cycles', [1, 2, 3, 4, 0]),
                              ('direction_cycles', [10, 0, 0, 0, 0])):
            self.data['tx_observations'][name] = metric(buckets, semantics=('cycle', 'cpu', 'observed_state_duration'))
        # These duplicates are intentional: the writer preserves them.
        self.data['metrics'] += [dict(name='stop_unknown', **metric(2)), dict(name='stop_unknown', **metric(9)),
                                 dict(name='wait_unknown_ticks', **metric(3, semantics=('tick', 'sst', 'interval_sum'))),
                                 dict(name='wait_unknown_ticks', **metric(8, semantics=('tick', 'sst', 'interval_sum')))]
        self.unavailable([n for n in checker.METRICS if n.startswith('memory_')])
        self.csv_override = None
        self.payloads = {}

    def named(self, name):
        return next(row for row in self.data['metrics'] if row['name'] == name)

    def config_value(self, key, value):
        self.config['parameters'][key]['value'] = value

    def unavailable(self, names, status='disabled'):
        for name in names:
            self.named(name).update(value=None, status=status)

    def save(self):
        self.path.write_text(json.dumps(self.data))
        self.config_path.write_text(json.dumps(self.config))
        rows = [['metric', 'value']] + [[row['name'], row['value'] if row['status'] == 'available'
                                       else self.payloads.get(row['name'], 0)] for row in self.data['metrics']]
        with self.path.with_suffix('.csv').open('w', newline='') as stream:
            csv.writer(stream).writerows(self.csv_override if self.csv_override is not None else rows)

    def good(self, identity=IDENTITY):
        self.save()
        count, errors = checker.check_roots([self.root], identity)
        self.assertEqual((count, errors), (1, []))

    def bad(self, diagnostic):
        self.save()
        count, errors = checker.check_roots([self.root], IDENTITY)
        self.assertEqual(count, 1)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn(diagnostic, errors[0])

    def test_valid_mixed_clocks_and_duplicates(self):
        self.good()

    def test_relative_config_reference(self):
        self.data['metadata']['resolved_config_reference'] = optional('resolved/tile-7.json')
        self.good()

    def test_resources_disabled_are_null_not_zero(self):
        self.config_value('analog_array_count', 0)
        self.config_value('launch_mode', 'disabled')
        self.config_value('profile_mode', 'summary')
        # network_size stays nonzero: it is NOT proof of attachment.
        names = [n for n in checker.METRICS if n.startswith(('analog_', 'memory_', 'network_', 'transmit_', 'physical_global_dma_'))]
        names += ['receive_dma_active_cycles', 'instructions', 'vector_instructions', 'cpu_cycles',
                  'synchronization_grants', 'synchronization_events']
        self.unavailable(names)
        self.unavailable(['profile_clock_regressions'], 'not_measured')
        for row in self.data['metrics'][len(checker.METRICS):]:
            row.update(value=None, status='disabled')
        for domain in ('rx', 'analog'):
            self.data['timebase']['domain_factors'][domain] = optional(None)
        for name in ('observed_cycles', 'ready_cycles', 'active_cycles', 'direction_cycles'):
            self.data['tx_observations'][name].update(value=None, status='disabled')
        self.good()

    def test_t4_unmeasured_keeps_legacy_payload(self):
        self.config_value('tx_dma_streams', 4)
        names = ['network_endpoint_queue_ticks', 'transmit_blocked_ticks', 'transmit_blocked_events',
                 'transmit_blocked_retries', 'transmit_maximum_queue_occupancy']
        self.unavailable(names, 'not_measured')
        self.payloads = {name: 37 for name in names}
        self.good()

    def test_available_measured_zero_histograms(self):
        self.data['tx_observations']['observed_cycles']['value'] = 0
        for name in ('ready_cycles', 'active_cycles', 'direction_cycles'):
            self.data['tx_observations'][name]['value'] = [0] * 5
        self.good()

    def test_missing_or_unsupported_schema(self):
        for field, value in (('schema', 'wrong'), ('schema_version', 2), ('schema_version', True), ('contract', 'wrong')):
            with self.subTest(field=field, value=value):
                before = self.data[field]
                self.data[field] = value
                self.bad('schema' if field != 'contract' else 'contract')
                self.data[field] = before

    def test_implementation_mismatch(self):
        self.data['metadata']['implementation_id'] = optional('sha256:' + 'b' * 64)
        self.bad('implementation_id mismatch')

    def test_unknown_or_empty_provenance(self):
        for name in ('source_tree', 'implementation_id', 'resolved_config_reference'):
            with self.subTest(name=name):
                before = copy.deepcopy(self.data['metadata'][name])
                self.data['metadata'][name] = optional(None)
                self.bad('provenance is required')
                self.data['metadata'][name] = optional('')
                self.bad('nonempty text')
                self.data['metadata'][name] = before

    def test_config_missing_wrong_tile_or_version(self):
        self.data['metadata']['resolved_config_reference'] = optional('missing.json')
        self.bad('does not exist')
        self.data['metadata']['resolved_config_reference'] = optional(str(self.config_path))
        self.config_value('tile_id', 8)
        self.bad('tile_id does not match')
        self.config_value('tile_id', 7)
        self.config['schema_version'] = 2
        self.bad('config schema_version')

    def test_status_and_null_corruption(self):
        row = self.named('instructions')
        for value, status in ((None, 'available'), (0, 'disabled'), (0, 'unknown'), (0, 'not_measured'), (1, 'invented')):
            with self.subTest(value=value, status=status):
                row.update(value=value, status=status)
                self.bad('metrics[2]')

    def test_numeric_corruption(self):
        for value in (-1, True, 1.0, '1', 1 << 64):
            with self.subTest(value=value):
                self.named('instructions')['value'] = value
                self.bad('uint64')

    def test_available_csv_mismatch(self):
        self.save()
        with self.path.with_suffix('.csv').open() as stream:
            self.csv_override = list(csv.reader(stream))
        self.csv_override[3][1] = '51'
        self.bad('available value differs from CSV')

    def test_duplicate_rows_cannot_be_collapsed_or_reordered(self):
        self.save()
        with self.path.with_suffix('.csv').open() as stream:
            original = list(csv.reader(stream))
        self.csv_override = copy.deepcopy(original)
        self.csv_override[-4], self.csv_override[-3] = self.csv_override[-3], self.csv_override[-4]
        self.bad('available value differs from CSV')
        self.csv_override = original[:-1]
        self.bad('row count differs')

    def test_both_formats_missing_fixed_metric(self):
        self.data['metrics'].pop(2)
        self.bad('fixed schema row missing or reordered')

    def test_units_and_scope_required(self):
        self.named('cpu_cycles')['domain'] = 'sst'
        self.bad('incorrect unit/domain/aggregation')
        self.named('cpu_cycles')['domain'] = 'cpu'
        self.named('cpu_cycles')['known_scope'] = ''
        self.bad('known_scope')

    def test_disabled_resource_cannot_report_measured_zero(self):
        self.config_value('analog_array_count', 0)
        self.named('analog_commands_submitted')['value'] = 0
        self.named('analog_commands_completed')['value'] = 0
        self.bad('analog_commands_submitted: expected disabled')

    def test_t4_partial_metrics_not_available(self):
        self.config_value('tx_dma_streams', 4)
        self.bad('expected not_measured')

    def test_wrong_and_zero_clock_factors(self):
        factors = self.data['timebase']['domain_factors']
        for domain in ('cpu', 'rx', 'analog'):
            with self.subTest(domain=domain):
                before = factors[domain]['value']
                factors[domain]['value'] = before + 1
                self.bad(f'factor.{domain}: differs')
                factors[domain]['value'] = 0
                self.bad('positive uint64')
                factors[domain]['value'] = before

    def test_external_clocks_must_not_be_inferred(self):
        for domain in ('nic', 'router', 'global_ram'):
            with self.subTest(domain=domain):
                self.data['timebase']['domain_factors'][domain] = optional(10000)
                self.bad('external component factor must remain unknown')
                self.data['timebase']['domain_factors'][domain] = optional(None)

    def test_unknown_factor_cannot_carry_zero(self):
        self.data['timebase']['domain_factors']['nic']['value'] = 0
        self.bad('unknown must have null value')

    def test_sst_rounding_and_period_units(self):
        self.assertEqual(checker.expected_factor('1.5GHz', '1ps'), 667)
        self.assertEqual(checker.expected_factor('2.5 ns', '1ns'), 2)
        self.assertEqual(checker.expected_factor('3.5ns', '1ns'), 4)
        self.assertEqual(checker.expected_factor('0.5 us', '1ps'), 500000)
        for value in ('0Hz', 'bananas', '-1ns', '1GB'):
            with self.subTest(value=value), self.assertRaises(checker.ValidationError):
                checker.expected_factor(value, '1ps')

    def test_every_histogram_conserves_including_bucket_zero(self):
        for name in ('ready_cycles', 'active_cycles', 'direction_cycles'):
            with self.subTest(name=name):
                self.data['tx_observations'][name]['value'][0] += 1
                self.bad('all buckets must sum')
                self.data['tx_observations'][name]['value'][0] -= 1

    def test_histogram_overflow_and_missing_bucket(self):
        hist = self.data['tx_observations']['ready_cycles']
        hist['value'] = [checker.UINT64_MAX, 1, 0, 0, 0]
        self.bad('all buckets must sum')
        hist['value'] = [1, 3, 0, 4]
        self.bad('all five histogram buckets')
        hist['value'] = [2, True, 3, 0, 4]
        self.bad('uint64')

    def test_histogram_requires_available_window(self):
        self.data['tx_observations']['observed_cycles'].update(value=None, status='unknown')
        self.bad('TX window availability mismatch')

    def test_spm_and_completion_invariants(self):
        self.named('scratchpad_read_service_cycles')['value'] = 6
        self.bad('SPM read/write service')
        self.named('scratchpad_read_service_cycles')['value'] = 5
        for name in ('physical_global_dma_completed', 'analog_commands_completed'):
            with self.subTest(name=name):
                before = self.named(name)['value']
                self.named(name)['value'] = 99
                self.bad('exceeds submissions')
                self.named(name)['value'] = before

    def test_missing_csv_and_duplicate_json_keys(self):
        self.save()
        self.path.with_suffix('.csv').unlink()
        self.assertEqual(len(checker.check_roots([self.root])[1]), 1)
        self.save()
        self.path.write_text(self.path.read_text().replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1', 1))
        self.assertIn('duplicate JSON key', checker.check_roots([self.root])[1][0])

    def test_roots_discovery_and_cli(self):
        with self.assertRaisesRegex(checker.ValidationError, 'no tile-N-summary'):
            checker.check_roots([self.root])
        with self.assertRaisesRegex(checker.ValidationError, 'does not exist'):
            checker.check_roots([self.root / 'absent'])
        self.save()
        self.assertEqual(checker.check_roots([self.root, self.directory]), (1, []))
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(checker.main([str(self.root), '--implementation-id', IDENTITY]), 0)
            self.assertEqual(checker.main([str(self.root), '--implementation-id', 'wrong']), 1)


if __name__ == '__main__':
    unittest.main()
