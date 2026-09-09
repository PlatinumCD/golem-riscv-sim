#!/usr/bin/env python3
"""Host-only checks of exact comparison semantics."""
import unittest
from pathlib import Path
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from comparison import REPORT, differences, fingerprint, observations


class ComparisonTests(unittest.TestCase):
    def test_both_values_survive(self):
        self.assertEqual(differences({'x': 1}, {'x': 2}),
                         [{'file': 'x', 'reference': 1, 'candidate': 2}])

    def test_print_order_does_not_change_observation(self):
        self.assertEqual(fingerprint(['tile0', 'tile1']),
                         fingerprint(['tile1', 'tile0']))

    def test_duplicate_observations_are_not_discarded(self):
        self.assertNotEqual(fingerprint(['tile0']), fingerprint(['tile0', 'tile0']))

    def test_timeline_order_is_significant(self):
        self.assertNotEqual(fingerprint(['submit', 'complete'], preserve_order=True),
                            fingerprint(['complete', 'submit'], preserve_order=True))

    def test_missing_does_not_mean_zero(self):
        self.assertEqual(len(differences({'x': 0}, {})), 1)

    def test_report_preserves_every_numeric_field(self):
        report = 'MITTENS_TX_OPPORTUNITY tile=0 tx_streams=4 active_4_cycles=125'
        self.assertEqual(REPORT.search('tile.cc:34 ' + report + '\n')[1], report)
        self.assertNotEqual(fingerprint([report]), fingerprint([report[:-1] + '6']))

    def test_host_progress_is_not_a_hardware_report(self):
        self.assertIsNone(REPORT.search('MITTENS_PROGRESS tile=0 wall_ms=32'))

    def test_final_progress_preserves_hardware_fields_not_host_sampling(self):
        with tempfile.TemporaryDirectory(prefix='golem-progress-comparison-') as temporary:
            root = Path(temporary)
            path = root / 'tile-0-progress.csv'
            header = 'tile_id,kind,wall_time_ms,simulation_tick,instructions\n'
            path.write_text(header + '0,periodic,10,40,2\n0,final,20,100,5\n')
            original = observations(root)
            path.write_text(header + '0,final,30,100,5\n')
            self.assertEqual(original, observations(root))
            path.write_text(header + '0,final,30,101,5\n')
            self.assertNotEqual(original, observations(root))
            path.write_text(header)
            self.assertNotEqual(original, observations(root))


if __name__ == '__main__':
    unittest.main()
