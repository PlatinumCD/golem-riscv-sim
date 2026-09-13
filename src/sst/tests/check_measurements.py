#!/usr/bin/env python3
"""Read-only validation of Tile JSON summaries and neighboring CSV.

Usage: check_measurements.py ARTIFACT_ROOT [ARTIFACT_ROOT ...]
                              [--implementation-id sha256:...]

Requires named Tile snapshots, including their resolved configuration reference;
legacy positional-writer fixtures with unknown provenance are not real Tile
measurement evidence. Relative config references resolve beside the summary,
never by guessing from the current directory. Duplicate CSV metric names are
compared in order, not collapsed into a dictionary. No simulator is invoked.
"""
import argparse
import csv
from fractions import Fraction
import json
from pathlib import Path
import re
import sys

UINT64_MAX = (1 << 64) - 1
SUMMARY_NAME = re.compile(r'tile-(\d+)-summary\.json\Z')
STATUSES = {'available', 'disabled', 'not_measured', 'unknown'}
# Ordered schema-v1 fixed rows from MeasurementWriter::rows. Dynamic stop/wait
# rows follow these, retaining duplicates and their original relative order.
METRICS = {
    'tile_id': ('identifier', 'none', 'identity'),
    'finish_tick': ('tick', 'sst', 'timestamp'),
    'instructions': ('instruction', 'guest', 'count'),
    'vector_instructions': ('instruction', 'guest', 'count'),
    'cpu_cycles': ('cycle', 'cpu', 'issue_cost_sum'),
    'synchronization_grants': ('grant', 'none', 'count'),
    'synchronization_events': ('event', 'none', 'count'),
    'network_packets': ('packet', 'none', 'count'),
    'network_words': ('word_32bit', 'none', 'count'),
    'network_word_hops': ('word_32bit_hop', 'none', 'sum'),
    'network_transit_ticks': ('tick', 'sst', 'interval_sum'),
    'network_endpoint_queue_ticks': ('tick', 'sst', 'interval_sum'),
    'physical_global_dma_submitted': ('request', 'none', 'count'),
    'physical_global_dma_completed': ('request', 'none', 'count'),
    'analog_commands_submitted': ('command', 'none', 'count'),
    'analog_commands_completed': ('command', 'none', 'count'),
    'analog_active_cycles': ('cycle', 'analog', 'advanced_device_cycles'),
    'analog_link_beats': ('beat', 'analog', 'count'),
    'receive_dma_active_cycles': ('cycle', 'cpu', 'service_sum'),
    'transmit_dma_active_cycles': ('cycle', 'cpu', 'service_sum'),
    'scratchpad_service_cycles': ('cycle', 'cpu', 'service_sum'),
    'scratchpad_read_service_cycles': ('cycle', 'cpu', 'service_sum'),
    'scratchpad_write_service_cycles': ('cycle', 'cpu', 'service_sum'),
    'scratchpad_bank_conflicts': ('delayed_beat', 'none', 'count'),
    'scratchpad_queue_cycles': ('cycle', 'cpu', 'interval_sum'),
    'scratchpad_cpu_requests': ('access', 'none', 'count'),
    'scratchpad_dma_transfers': ('schedule_call', 'none', 'count'),
    'scratchpad_dma_bytes': ('byte', 'none', 'sum'),
    'transmit_blocked_ticks': ('tick', 'sst', 'interval_sum'),
    'transmit_blocked_events': ('episode', 'none', 'count'),
    'transmit_blocked_retries': ('attempt', 'none', 'count'),
    'transmit_maximum_queue_occupancy': ('queue_entry', 'none', 'maximum'),
    'memory_maximum_outstanding_requests': ('request', 'none', 'maximum'),
    'memory_maximum_outstanding_reads': ('request', 'none', 'maximum'),
    'memory_maximum_store_buffer_occupancy': ('entry', 'none', 'maximum'),
    'memory_store_buffer_full_events': ('event', 'none', 'count'),
    'memory_vector_request_groups': ('group', 'none', 'count'),
    'memory_vector_group_requests': ('request', 'none', 'count'),
    'memory_scalar_request_groups': ('group', 'none', 'count'),
    'memory_scalar_group_requests': ('request', 'none', 'count'),
    'profile_clock_regressions': ('event', 'none', 'count'),
    'progress_snapshots': ('snapshot', 'none', 'count'),
    'progress_watchdog_events': ('snapshot', 'none', 'count'),
    'progress_counter_flushes': ('checkpoint', 'none', 'count'),
}


class ValidationError(ValueError):
    """Invalid or incomplete measurement evidence, not a simulation failure."""


def require(condition, message):
    if not condition:
        raise ValidationError(message)


def text(value, where):
    require(isinstance(value, str) and bool(value.strip()), f'{where}: expected nonempty text')
    return value


def uint(value, where, *, positive=False):
    require(type(value) is int and (1 if positive else 0) <= value <= UINT64_MAX,
            f'{where}: expected {"positive " if positive else ""}uint64')
    return value


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path):
    with path.open() as stream:
        return json.load(stream, object_pairs_hook=object_pairs,
                         parse_constant=lambda value: (_ for _ in ()).throw(
                             ValidationError(f'nonfinite JSON value: {value}')))


def observation(node, where, *, kind='integer', optional=False):
    require(isinstance(node, dict), f'{where}: expected value/status object')
    require('value' in node and 'status' in node, f'{where}: missing value/status')
    status = node['status']
    allowed = {'available', 'unknown'} if optional else STATUSES
    require(isinstance(status, str) and status in allowed, f'{where}: invalid status {status!r}')
    value = node['value']
    if status != 'available':
        require(value is None, f'{where}: {status} must have null value, not a numeric placeholder')
    elif kind == 'text':
        text(value, where)
    elif kind == 'histogram':
        require(isinstance(value, list) and len(value) == 5, f'{where}: expected all five histogram buckets')
        for i, item in enumerate(value):
            uint(item, f'{where}[{i}]')
    else:
        uint(value, where, positive=kind == 'factor')
    return value


def semantics(node, expected, where):
    require(tuple(node.get(key) for key in ('unit', 'domain', 'aggregation')) == expected,
            f'{where}: incorrect unit/domain/aggregation; expected {expected}')
    text(node.get('known_scope'), f'{where}.known_scope')


def period_seconds(value, where):
    """Exact SI frequency/period parsing for SST clock/timebase text."""
    match = re.fullmatch(r'\s*([+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*([A-Za-zµμ]+)\s*',
                         text(value, where))
    require(match is not None, f'{where}: unsupported clock/timebase {value!r}')
    number, unit = match.groups()
    frequency = unit.endswith('Hz')
    require(frequency or unit.endswith('s'), f'{where}: expected SI Hz or seconds')
    prefix = unit[:-2] if frequency else unit[:-1]
    powers = {'a': -18, 'f': -15, 'p': -12, 'n': -9, 'u': -6, 'µ': -6, 'μ': -6,
              'm': -3, 'c': -2, 'd': -1, '': 0, 'da': 1, 'h': 2, 'k': 3, 'K': 3,
              'M': 6, 'G': 9, 'T': 12, 'P': 15, 'E': 18}
    require(prefix in powers, f'{where}: unsupported SI prefix {prefix!r}')
    magnitude = Fraction(number) * Fraction(10) ** powers[prefix]
    require(magnitude > 0, f'{where}: clock/timebase must be positive')
    return 1 / magnitude if frequency else magnitude


def expected_factor(clock, timebase):
    ratio = period_seconds(clock, 'configured clock') / period_seconds(timebase, 'sst_timebase')
    require(ratio >= 1, 'configured clock: period is shorter than one SST tick')
    # TimeLord::getFactorForTime -> UnitAlgebra::getRoundedValue ->
    # decimal_fixedpoint::toLong uses nearest-even, NOT floor or ceiling.
    return uint(round(ratio), 'configured converter factor', positive=True)


def validate_summary(path, implementation_id=None):
    path = Path(path)
    match = SUMMARY_NAME.fullmatch(path.name)
    require(match is not None, 'expected tile-N-summary.json filename')
    tile = int(match[1])
    data = read_json(path)
    require(isinstance(data, dict), 'summary must be an object')
    require(data.get('schema') == 'mittens.summary', 'wrong summary schema')
    require(type(data.get('schema_version')) is int and data['schema_version'] == 1,
            'unsupported summary schema_version')
    require(data.get('contract') == 'src/sst/profiling/MEASUREMENT_CONTRACT.md', 'wrong contract reference')
    for key in ('legacy_csv_policy', 'known_scope'):
        text(data.get(key), key)
    metadata = data['metadata']
    for key in ('source_tree', 'implementation_id', 'build_id', 'run_manifest_reference', 'resolved_config_reference'):
        observation(metadata[key], f'metadata.{key}', kind='text', optional=True)
    for key in ('source_tree', 'implementation_id', 'resolved_config_reference'):
        require(metadata[key]['status'] == 'available', f'metadata.{key}: actual Tile provenance is required')
    identity = metadata['implementation_id']['value']
    require(re.fullmatch(r'sha256:[0-9a-f]{64}', identity) is not None, 'invalid implementation_id')
    if implementation_id is not None:
        require(identity == implementation_id, f'implementation_id mismatch: {identity}')
    config_path = Path(metadata['resolved_config_reference']['value'])
    if not config_path.is_absolute():
        config_path = path.parent / config_path
    require(config_path.is_file(), f'resolved config reference does not exist: {config_path}')
    config = read_json(config_path)
    require(type(config.get('schema_version')) is int and config['schema_version'] == 1,
            'unsupported resolved config schema_version')
    parameters = config['parameters']

    def param(name):
        entry = parameters[name]
        text(entry['category'], f'config.{name}.category')
        return entry['value']

    require(uint(param('tile_id'), 'config.tile_id') == tile, 'resolved config tile_id does not match filename')
    rows = data['metrics']
    require(isinstance(rows, list) and len(rows) >= len(METRICS), 'metrics: missing fixed schema rows')
    with path.with_suffix('.csv').open(newline='') as stream:
        csv_rows = list(csv.reader(stream, strict=True))
    require(csv_rows and csv_rows[0] == ['metric', 'value'], 'legacy CSV: wrong header')
    require(len(csv_rows) == len(rows) + 1, 'legacy CSV: row count differs from JSON')
    fixed = {}
    seen_wait = False
    for index, (row, legacy) in enumerate(zip(rows, csv_rows[1:])):
        where = f'metrics[{index}]'
        value = observation(row, where)
        name = text(row.get('name'), f'{where}.name')
        require(len(legacy) == 2 and legacy[0] == name, f'{where}: ordered legacy CSV name mismatch')
        require(re.fullmatch(r'[0-9]+', legacy[1]) is not None, f'{where}: invalid legacy CSV integer')
        legacy_value = uint(int(legacy[1]), where + '.csv')
        if value is not None:
            require(value == legacy_value, f'{where} ({name}): available value differs from CSV')
        if index < len(METRICS):
            require(name == list(METRICS)[index], f'{where}: fixed schema row missing or reordered')
            expected = METRICS[name]
            fixed[name] = row
        elif name.startswith('stop_'):
            require(not seen_wait, f'{where}: stop row after wait rows')
            expected = ('event', 'none', 'count')
        else:
            require(name.startswith('wait_') and name.endswith('_ticks'), f'{where}: unknown schema metric {name}')
            seen_wait = True
            expected = ('tick', 'sst', 'interval_sum')
        semantics(row, expected, where)
    require(fixed['tile_id']['value'] == tile, 'JSON tile_id does not match filename')

    def expect(names, status):
        for name in names:
            require(fixed[name]['status'] == status, f'{name}: expected {status}, got {fixed[name]["status"]}')

    def enabled_status(enabled):
        return 'available' if enabled else 'disabled'

    launch = param('launch_mode')
    require(launch in ('managed', 'disabled'), 'invalid configured launch_mode')
    cpu = launch == 'managed'
    # Every tile has SPM. There is no separate CPU data-cache transport;
    # its retained schema fields must be unavailable, never measured zero.
    memory = False
    scratchpad = True
    analog = uint(param('analog_array_count'), 'config.analog_array_count') != 0
    # Attachment is not inferable from network_size or global RAM capacity.
    network_status = fixed['network_packets']['status']
    require(network_status in ('available', 'disabled'), 'network availability is not established')
    network = network_status == 'available'
    require(not network or uint(param('network_size'), 'config.network_size') > 0,
            'attached network requires nonzero network_size')
    dma_status = fixed['physical_global_dma_submitted']['status']
    require(dma_status in ('available', 'disabled'), 'globalDMA availability is not established')
    expect(['tile_id', 'finish_tick', 'progress_snapshots', 'progress_watchdog_events', 'progress_counter_flushes'], 'available')
    expect(['instructions', 'vector_instructions', 'cpu_cycles', 'synchronization_grants', 'synchronization_events'], enabled_status(cpu))
    for row in rows[len(METRICS):]:
        require(row['status'] == enabled_status(cpu), f'{row["name"]}: CPU stop/wait availability mismatch')
    expect(['network_packets', 'network_words', 'network_word_hops', 'network_transit_ticks', 'receive_dma_active_cycles'], network_status)
    expect(['transmit_dma_active_cycles'], enabled_status(network and scratchpad))
    expect([name for name in METRICS if name.startswith('scratchpad_')], enabled_status(scratchpad))
    expect([name for name in METRICS if name.startswith('memory_')], enabled_status(memory))
    expect([name for name in METRICS if name.startswith('analog_')], enabled_status(analog))
    expect(['physical_global_dma_submitted', 'physical_global_dma_completed'], dma_status)
    streams = uint(param('tx_dma_streams'), 'config.tx_dma_streams', positive=True)
    require(streams in (1, 2, 4), 'invalid configured TX stream count')
    partial_status = 'disabled' if not network else 'available' if streams == 1 else 'not_measured'
    expect(['network_endpoint_queue_ticks', 'transmit_blocked_ticks', 'transmit_blocked_events',
            'transmit_blocked_retries', 'transmit_maximum_queue_occupancy'], partial_status)
    expect(['profile_clock_regressions'], 'available' if param('profile_mode') == 'trace' else 'not_measured')

    timebase = data['timebase']
    base = observation(timebase['sst_timebase'], 'sst_timebase', kind='text', optional=True)
    require(base is not None, 'actual Tile SST timebase must be available')
    require(timebase['factor_unit'] == 'sst_ticks_per_domain_cycle', 'incorrect factor_unit')
    factors = timebase['domain_factors']
    for domain in ('cpu', 'rx', 'analog', 'nic', 'router', 'global_ram'):
        observation(factors[domain], f'factor.{domain}', kind='factor', optional=True)
    for domain in ('nic', 'router', 'global_ram'):
        require(factors[domain]['status'] == 'unknown', f'factor.{domain}: external component factor must remain unknown')
    for domain, key, present in (('cpu', 'cpu_clock', True), ('rx', 'rx_dma_clock', network),
                                 ('analog', 'analog_link_clock', analog)):
        require(factors[domain]['status'] == ('available' if present else 'unknown'), f'factor.{domain}: availability mismatch')
        if present:
            require(factors[domain]['value'] == expected_factor(param(key), base), f'factor.{domain}: differs from configured clock/SST timebase')

    tx = data['tx_observations']
    semantics(tx, ('cycle', 'cpu', 'observed_state_duration'), 'tx_observations')
    require(tx['bucket_labels'] == [0, 1, 2, 3, '4+'] and
            all(type(v) is int for v in tx['bucket_labels'][:4]), 'TX bucket_labels must include zero through 4+')
    window = observation(tx['observed_cycles'], 'tx.observed_cycles')
    semantics(tx['observed_cycles'], ('cycle', 'cpu', 'observation_window_duration'), 'tx.observed_cycles')
    require(tx['observed_cycles']['status'] == network_status, 'TX window availability mismatch')
    for name in ('ready_cycles', 'active_cycles', 'direction_cycles'):
        histogram = observation(tx[name], f'tx.{name}', kind='histogram')
        semantics(tx[name], ('cycle', 'cpu', 'observed_state_duration'), f'tx.{name}')
        require(tx[name]['status'] == network_status, f'tx.{name}: histogram availability mismatch')
        if histogram is not None:
            require(window is not None and sum(histogram) == window, f'tx.{name}: all buckets must sum to observation window')
    for submitted, completed in (('physical_global_dma_submitted', 'physical_global_dma_completed'),
                                 ('analog_commands_submitted', 'analog_commands_completed')):
        if fixed[submitted]['status'] == 'available':
            require(fixed[completed]['value'] <= fixed[submitted]['value'], f'{completed}: exceeds submissions')
    if scratchpad:
        require(fixed['scratchpad_read_service_cycles']['value'] + fixed['scratchpad_write_service_cycles']['value'] ==
                fixed['scratchpad_service_cycles']['value'], 'SPM read/write service does not reconcile')


def check_roots(roots, implementation_id=None):
    paths = set()
    for root in map(Path, roots):
        require(root.is_dir(), f'artifact root does not exist or is not a directory: {root}')
        paths.update(path.resolve() for path in root.rglob('tile-*-summary.json')
                     if path.is_file() and SUMMARY_NAME.fullmatch(path.name))
    require(bool(paths), 'no tile-N-summary.json found in artifact roots')
    errors = []
    for path in sorted(paths):
        try:
            validate_summary(path, implementation_id)
        except (OSError, ValueError, KeyError, TypeError, IndexError, csv.Error) as error:
            errors.append(f'{path}: {error}')
    return len(paths), errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact_roots', nargs='+', type=Path)
    parser.add_argument('--implementation-id')
    args = parser.parse_args(argv)
    try:
        count, errors = check_roots(args.artifact_roots, args.implementation_id)
    except (OSError, ValidationError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    for error in errors:
        print(f'FAIL: {error}', file=sys.stderr)
    print(f'{"FAIL" if errors else "PASS"}: {count} summaries checked; {len(errors)} invalid')
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
