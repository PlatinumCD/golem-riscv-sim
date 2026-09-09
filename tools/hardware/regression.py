#!/usr/bin/env python3
"""Compare explicit hardware baselines using the normal hardware case registry.

Historical baseline schemas are preserved. Current candidate measurements must
validate; comparisons retain CSV values and ordered observations, not metadata.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import time
from hardware_paths import resolve_paths
from hardware_runner import BINARY_NAMES, counters, digest, run_cases, save
from hardware_suite import selected_cases
from comparison import observations, differences


def validate_baseline(install):
    manifest_path = install.parent / 'manifest.json'
    if not manifest_path.is_file():
        raise ValueError(f'baseline requires pinned provenance: {manifest_path}')
    manifest = json.loads(manifest_path.read_text())
    for name in BINARY_NAMES:
        expected = manifest.get('binary_sha256', {}).get(name)
        if expected is None or digest(install / name) != expected:
            raise ValueError(f'baseline binary does not match pinned manifest: {install / name}')
    return manifest_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-install', type=Path, required=True, help='Explicit pinned baseline installation')
    parser.add_argument('--candidate-install', type=Path, help='Defaults to selected hardware installation')
    parser.add_argument('--reference-tree', choices=('src',), default='src', help='Both arms use current guest/component sources')
    parser.add_argument('--case', action='append')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    try:
        paths = resolve_paths()
        reference = args.reference_install.resolve()
        baseline = validate_baseline(reference)
        candidate = (args.candidate_install or Path(paths['GOLEM_INSTALL_ROOT'])).resolve()
        for name in BINARY_NAMES:
            if not (candidate / name).is_file():
                raise ValueError(f'missing candidate binary: {candidate / name}')
        cases = selected_cases('hardware', args.case)
        if args.timeout <= 0:
            raise ValueError('--timeout must be positive')
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    output = Path(paths['GOLEM_BUILD_ROOT']) / 'regressions' / str(time.time_ns())
    output.mkdir(parents=True)
    record = {'reference_manifest': str(baseline), 'reference_installation': str(reference),
              'candidate_installation': str(candidate), 'cases': [case.name for case in cases],
              'source_policy': 'same current guest/component sources; explicitly pinned reference hardware',
              'excluded': ['compiler/model suites', 'materialized-functional', 'study sweeps', 'metadata equality']}

    def arm(label):
        return run_cases(cases, reference if label == 'reference' else candidate,
                         output / label, args.timeout, historical_baseline=label == 'reference')

    with ThreadPoolExecutor(max_workers=2) as pool:
        reports = list(pool.map(arm, ('reference', 'candidate')))
    counter_differences = differences(counters(output / 'reference/cases'), counters(output / 'candidate/cases'))
    observation_differences = differences(observations(output / 'reference/cases'), observations(output / 'candidate/cases'))
    record.update(arms=reports, counter_differences=counter_differences,
                  hardware_observation_differences=observation_differences)
    record['status'] = ('PASS' if all(report['status'] == 'PASS' for report in reports)
                        and not counter_differences and not observation_differences else 'FAIL')
    save(output / 'comparison.json', record)
    print(f'{record["status"]}: {len(counter_differences)} counter differences, '
          f'{len(observation_differences)} observation differences; {output / "comparison.json"}', flush=True)
    return int(record['status'] != 'PASS')


if __name__ == '__main__':
    raise SystemExit(main())
