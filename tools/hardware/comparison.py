"""Deterministic hardware observations, excluding host progress/timing output."""
import csv
import hashlib
import json
from pathlib import Path
import re


REPORT = re.compile(
    r'\b(MITTENS_(?:PROFILE(?:_[A-Z]+)?|TX_OPPORTUNITY|ANALOG_ACTIVITY) tile=[^\r\n]*)')


TRACE_NAME = re.compile(
    r'tile-\d+(?:-(?:waits|network|receive-dma|analog|memory|transmit-blocked))?\.csv$')
PROGRESS_NAME = re.compile(r'tile-\d+-progress\.csv$')


def fingerprint(rows, *, preserve_order=False):
    # SST component finalization order is unspecified. Keep multiplicity but
    # compare resource records independently of their print/CSV row order.
    ordered = list(rows) if preserve_order else sorted(rows)
    return {'records': len(ordered), 'sha256': hashlib.sha256(
        json.dumps(ordered, separators=(',', ':')).encode()).hexdigest()}


def observations(directory):
    result = {}
    for path in sorted(Path(directory).rglob('*')):
        if not path.is_file():
            continue
        relative = str(path.relative_to(directory))
        if path.suffix in ('.out', '.log'):
            # Remove source-file/line prefixes from Output::verbose, not fields.
            rows = []
            with path.open(errors='replace') as stream:
                for line in stream:
                    match = REPORT.search(line)
                    if match:
                        rows.append(match[1])
            if rows:
                result[relative] = fingerprint(rows)
        elif PROGRESS_NAME.fullmatch(path.name):
            with path.open() as stream:
                reader = csv.DictReader(stream)
                columns = [name for name in reader.fieldnames or [] if name != 'wall_time_ms']
                # Periodic/watchdog sampling follows host time. Final snapshots
                # are deterministic hardware observations except their wall clock.
                rows = [[row[name] for name in columns] for row in reader if row['kind'] == 'final']
            result[relative] = {'columns': columns, **fingerprint(rows, preserve_order=True)}
        elif (path.name.endswith('router-statistics.csv') or TRACE_NAME.fullmatch(path.name)
              or path.name == 'global-ram-requests.csv'):
            with path.open() as stream:
                rows = list(csv.reader(stream))
            if rows:
                # Per-resource timelines must retain their exact event order.
                result[relative] = {'columns': rows[0], **fingerprint(
                    rows[1:], preserve_order=not path.name.endswith('router-statistics.csv'))}
    return result


def differences(reference, comparison):
    return [{'file': path, 'reference': reference.get(path), 'candidate': comparison.get(path)}
            for path in sorted(reference.keys() | comparison.keys())
            if reference.get(path) != comparison.get(path)]


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Compare preserved hardware observations and event timelines.')
    parser.add_argument('reference', type=Path)
    parser.add_argument('comparison', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    left, right = observations(args.reference), observations(args.comparison)
    delta = differences(left, right)
    report = {'reference': str(args.reference), 'comparison': str(args.comparison),
              'reference_observations': left, 'comparison_observations': right,
              'differences': delta, 'status': 'PASS' if not delta else 'REVIEW_REQUIRED'}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{report["status"]}: {len(left)}/{len(right)} files, {len(delta)} differences')
    for difference in delta:
        print(difference['file'])
    raise SystemExit(1 if delta else 0)
