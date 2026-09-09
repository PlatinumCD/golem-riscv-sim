"""Measurement values carry availability, units and aggregation; never infer zero."""
import csv
import json
import math
from pathlib import Path


class MeasurementError(ValueError):
    pass


def metric(value, unit, aggregation, scope, *, reason=None):
    if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float))
                              or not math.isfinite(value) or value < 0):
        raise MeasurementError(f'invalid {scope}: {value}')
    if value is None and not reason:
        raise MeasurementError('missing values require an explanation')
    return dict(value=value, status='measured' if value is not None else 'not_measured',
                unit=unit, aggregation=aggregation, scope=scope, reason=reason)


def required(mapping, key):
    if key not in mapping or mapping[key] is None:
        raise MeasurementError(f'required measurement missing: {key}')
    return mapping[key]


def optional(mapping, key):
    return mapping[key] if key in mapping else None


def elapsed(start, finish):
    if finish < start:
        raise MeasurementError(f'timestamp reversal: {start} -> {finish}')
    return finish - start


def elapsed_metric(start, finish, scope):
    return metric(elapsed(start, finish), 'tick', 'elapsed_interval', scope)


def union_duration(intervals):
    end = None
    total = 0
    for start, finish in sorted(intervals):
        elapsed(start, finish)
        if end is None or start >= end:
            total += finish - start
        elif finish > end:
            total += finish - end
        end = max(finish, end) if end is not None else finish
    return total


def rate(byte_count, duration):
    if byte_count is None or duration is None or duration <= 0:
        return None
    return byte_count / duration


def csv_rows(path, *, required_file=True):
    path = Path(path)
    if not path.is_file():
        if required_file:
            raise MeasurementError(f'missing trace: {path}')
        return None
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def summary(path):
    document = json.loads(Path(path).read_text())
    values = {}
    for row in document['metrics']:
        if row['name'] in values:
            raise MeasurementError(f'duplicate metric: {row["name"]}')
        values[row['name']] = row['value'] if row['status'] == 'available' else None
    return document, values


def save(path, document):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + '\n')


def write_csv(path, rows):
    if not rows:
        raise MeasurementError('refusing empty CSV dataset')
    with Path(path).open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)  # None is an empty cell, not numeric zero.
