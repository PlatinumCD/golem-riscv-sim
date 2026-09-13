#!/usr/bin/env python3
"""Host-only runner selection, failure and artifact semantics tests."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from hardware_runner import execute, measurements, counters, run_cases
from hardware_suite import Case, GROUPS, HARDWARE, ROOT, selected_cases, runtime_cases


class HardwareRunnerTests(unittest.TestCase):
    def test_hardware_default_is_an_allowlist(self):
        cases = selected_cases()
        self.assertEqual(len(cases), 32)
        self.assertFalse(any(case.requires_runtime for case in cases))
        self.assertFalse(set(case.name for case in cases) & set(case.name for case in runtime_cases()))
        self.assertTrue(all(case.script.is_file() for case in cases))
        for case in cases:
            self.assertNotIn('/compiler/', str(case.script))
            self.assertNotIn('/models/', str(case.script))
            self.assertNotIn('/studies/', str(case.script))
            self.assertNotIn('materialized-functional', case.name)
        self.assertIn('validation/performance-profile', [case.name for case in cases])

    def test_compiler_and_models_require_explicit_suite(self):
        for suite in ('compiler', 'models'):
            cases = selected_cases(suite)
            self.assertTrue(cases)
            self.assertTrue(all(case.name.startswith(suite + '/') for case in cases))
            with self.assertRaises(ValueError):
                selected_cases('hardware', [cases[0].name])

    def test_selection_is_exact_and_deduplicated(self):
        self.assertEqual([case.name for case in selected_cases(names=['component', 'component'])], ['component'])
        with self.assertRaises(ValueError):
            selected_cases(names=['missing'])

    def test_entrypoint_list_does_not_run_tests(self):
        result = subprocess.run(['bash', str(ROOT / 'tests/run-all.sh'), '--list'], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [case.name for case in selected_cases()])

    def test_hardware_groups_use_the_authoritative_allowlist(self):
        for group, names in HARDWARE.items():
            self.assertEqual([case.name for case in selected_cases(group=group)],
                             [f'{group}/{name}' for name in names])
        with self.assertRaises(ValueError):
            selected_cases(group='runtime', names=['runtime/materialized-functional'])
        with self.assertRaises(ValueError):
            selected_cases(group='network', names=['memory/global-ram'])

    def test_group_wrapper_delegates_all_selection_without_running(self):
        for group in GROUPS:
            result = subprocess.run(['bash', str(ROOT / 'tests/run-group.sh'), group, '--list'],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [case.name for case in selected_cases(group=group)])
            if group != 'runtime':
                self.assertTrue(all(line.startswith(group + '/') for line in result.stdout.splitlines()))

    def test_runtime_integrations_are_explicit_and_retained(self):
        expected = {'runtime/rx-controller', 'runtime/library', 'runtime/deployment-pair', 'runtime/epoch-barrier',
                    'network/transmit-fanout', 'analog/distributed-matvec'}
        self.assertEqual({case.name for case in selected_cases(group='runtime')}, expected)
        self.assertEqual({case.name for case in selected_cases('runtime')}, expected)
        self.assertTrue(expected <= {case.name for case in selected_cases('all')})

    def test_runtime_selection_requires_external_source(self):
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {'GOLEM_SCULPTOR_SOURCE': ''}):
            root = Path(temporary)
            result = run_cases(runtime_cases(), root / 'hardware', root / 'out', 1)
            self.assertEqual(result['status'], 'FAIL')
            self.assertIn('GOLEM_SCULPTOR_SOURCE', result['preflight']['error'])

    def test_group_cannot_broaden_implicitly_to_another_suite(self):
        for arguments in (['unknown', '--list'], ['network', '--suite', 'all', '--list']):
            result = subprocess.run(['bash', str(ROOT / 'tests/run-group.sh'), *arguments],
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)

    def test_pass_failure_timeout_and_launch_error(self):
        with tempfile.TemporaryDirectory(prefix='golem-runner-unit-') as temporary:
            root = Path(temporary)
            for index, (script, expected) in enumerate((('pass', 'PASS'), ('raise SystemExit(7)', 'FAIL'),
                                                       ('import time; time.sleep(30)', 'TIMEOUT'))):
                result = execute([sys.executable, '-c', script], dict(os.environ), root / f'{index}.log', .2)
                self.assertEqual(result['status'], expected)
                self.assertTrue(Path(result['log']).is_file())
            result = execute([str(root / 'no-executable')], dict(os.environ), root / 'missing.log', 1)
            self.assertEqual(result['status'], 'FAIL')

    def test_missing_outputs_are_not_a_pass(self):
        with tempfile.TemporaryDirectory(prefix='golem-measurement-unit-') as temporary:
            root = Path(temporary)
            self.assertEqual(measurements(root, True)['status'], 'FAIL')
            self.assertEqual(measurements(root)['status'], 'NOT_APPLICABLE')
            (root / 'tile-0-summary.csv').write_text('metric,value\nx,1\nx,2\n')
            self.assertEqual(measurements(root)['status'], 'FAIL')
            self.assertEqual(counters(root)['tile-0-summary.csv'][1:], [['x', '1'], ['x', '2']])

    def test_dependency_failure_is_not_successful_skip(self):
        with tempfile.TemporaryDirectory(prefix='golem-preflight-unit-') as temporary:
            root = Path(temporary)
            result = run_cases([Case('dummy', root / 'dummy.sh')], root / 'missing-install', root / 'out', 1)
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(result['preflight']['status'], 'FAIL')
            self.assertEqual(result['cases'][0]['status'], 'SKIP')

    def test_failed_case_does_not_stop_independent_cases(self):
        with tempfile.TemporaryDirectory(prefix='golem-continuation-unit-') as temporary:
            root = Path(temporary)
            with patch('hardware_runner.source_fingerprints', return_value={}), \
                 patch('hardware_runner.digest', return_value='fixture-hash'), \
                 patch('hardware_runner.link_dependency'), \
                 patch('hardware_runner.execute', side_effect=[
                     {'status': 'PASS'},
                     {'status': 'FAIL', 'wall_seconds': 0}, {'status': 'PASS', 'wall_seconds': 0}]) as execute_case:
                result = run_cases([Case('first', root / 'first.sh'), Case('second', root / 'second.sh')],
                                   root / 'hardware', root / 'out', 1, historical_baseline=True)
                self.assertEqual(result['runtime_build']['status'], 'NOT_APPLICABLE')
                self.assertEqual(execute_case.call_args_list[0].args[1]['GOLEM_TEST_RESULTS_ROOT'],
                                 str(root / 'out/setup/tests'))
                self.assertFalse(any('build-runtime.sh' in str(call) for call in execute_case.call_args_list))
                for call, name in zip(execute_case.call_args_list[1:], ('first', 'second')):
                    self.assertEqual(call.args[1]['GOLEM_TEST_CASE_NAME'], name)
                    self.assertEqual(call.args[1]['GOLEM_BUILD_ROOT'],
                                     str(root / 'out/cases' / name / 'artifacts'))
                    self.assertEqual(call.args[1]['GOLEM_TEST_RESULTS_ROOT'],
                                     str(root / 'out/cases' / name / 'artifacts/tests'))
            self.assertEqual([case['status'] for case in result['cases']], ['FAIL', 'PASS'])
            self.assertEqual(result['status'], 'FAIL')

    def test_current_validation_is_mandatory_even_after_successful_execution(self):
        with tempfile.TemporaryDirectory(prefix='golem-validation-unit-') as temporary:
            root = Path(temporary)
            with patch('hardware_runner.source_fingerprints', return_value={}), \
                 patch('hardware_runner.digest', return_value='fixture-hash'), \
                 patch('hardware_runner.link_dependency'), \
                 patch('hardware_runner.execute', side_effect=lambda *a, **k: {'status': 'PASS', 'wall_seconds': 0}):
                result = run_cases([Case('needs-measurements', root / 'fixture.sh', True)],
                                   root / 'hardware', root / 'out', 1)
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(result['cases'][0]['execution_status'], 'PASS')
            self.assertEqual(result['cases'][0]['measurements']['status'], 'FAIL')

    def test_reference_modes_fail_early(self):
        for command in (['tools/hardware/verify.py', '--check-copy-equivalence'],
                        ['tools/hardware/regression.py'],
                        ['tools/hardware/regression.py', '--reference-install', '/nonexistent-golem-reference']):
            result = subprocess.run([sys.executable, '-B', *command], cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('error:', result.stderr)


if __name__ == '__main__':
    unittest.main()
