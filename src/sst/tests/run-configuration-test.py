#!/usr/bin/env python3
"""Check resolved defaults, supported combinations and rejection of invalid input."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    install = Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install/src'))
    build = Path(os.environ.get('GOLEM_BUILD_ROOT', ROOT / 'build/src'))
    build.mkdir(parents=True, exist_ok=True)
    if os.environ.get('GOLEM_TEST_CASE_NAME') == 'configuration':
        # The runner already gave this case a unique build root. Stable names
        # inside it let paired runs compare every observation without discarding
        # configuration output or normalizing arbitrary file paths.
        output = build / 'configuration'
        output.mkdir(exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix='configuration-', dir=build))
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith('MITTENS_CONFIGURATION_')
                   and key != 'GOLEM_RESOLVED_CONFIG_DIR'}
    cases = [
        ('defaults', {}, True),
        ('T2', {'tx_dma_streams': 2}, True),
        ('T4', {'tx_dma_streams': 4}, True),
        ('R2', {'rx_dma_streams': 2}, True),
        ('R4', {'rx_dma_streams': 4}, True),
        ('bad-rx-lanes', {'rx_dma_streams': 3}, False),
        ('clocks', {'cpu_clock': '2GHz', 'rx_dma_clock': '500MHz'}, True),
        ('SPM', {'scratchpad_enabled': True}, True),
        ('escaped', {'elf': 'line\n"quote"\\slash'}, True),
        ('bad-tx', {'tx_dma_streams': 3}, False),
        ('bad-issue', {'cpu_issue_width': 0}, False),
        ('bad-rvv', {'riscv_vector_length_bits': 192}, False),
        ('bad-rx-width', {'rx_dma_width_bits': 33}, False),
        ('bad-fifo', {'tx_dma_fifo_bytes': 31}, False),
        ('bad-rx-spm', {'scratchpad_enabled': True, 'rx_dma_width_bits': 128}, False),
        ('bad-batching', {'scratchpad_access_batching': True}, False),
        ('bad-mesh', {'mesh_width': 2, 'mesh_height': 1, 'network_size': 3}, False),
        ('bad-memory', {'memory_backend': 'unknown'}, False),
    ]
    expected_errors = {
        'bad-rx-lanes': 'rx_dma_streams must be 1, 2, or 4',
        'bad-tx': 'requires tx_dma_streams in {1,2,4}',
        'bad-issue': 'requires cpu_issue_width to be 1, 2, or 4',
        'bad-rvv': 'requires riscv_vector_length_bits to be a power',
        # RX is now constructed after centralized TileConfiguration validation.
        'bad-rx-width': 'requires rx_dma_width_bits to be a positive multiple of 32',
        'bad-fifo': 'requires tx_dma_fifo_bytes to be zero or a word-aligned',
        'bad-rx-spm': 'requires RX-DMA width/setup to match',
        'bad-batching': 'requires scratchpad_enabled when',
        'bad-mesh': 'do not match network_size',
        'bad-memory': 'has unsupported memory_backend',
    }
    for name, params, valid in cases:
        trial = output / name
        trial.mkdir()
        path = trial / 'input.json'
        path.write_text(json.dumps(params))
        env = dict(environment, MITTENS_CONFIGURATION_INPUT=str(path),
                   GOLEM_RESOLVED_CONFIG_DIR=str(trial / 'resolved'),
                   SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'))
        with (trial / 'simulation.log').open('w') as log:
            process = subprocess.run([str(install / 'sst-core/bin/sst'),
                                      str(Path(__file__).with_name('configuration.py'))],
                                     env=env, stdout=log, stderr=subprocess.STDOUT, timeout=20)
        assert (process.returncode == 0) == valid, (name, process.returncode, trial)
        if not valid:
            assert expected_errors[name] in (trial / 'simulation.log').read_text(), name
        if valid:
            files = list((trial / 'resolved').glob('*.json'))
            assert len(files) == 1, files
            settings = json.loads(files[0].read_text())['parameters']
            assert len(settings) == 75, len(settings)
            for key, value in params.items():
                assert settings[key]['value'] == value, (name, key)
            assert settings['sync_instruction_quantum']['category'] == 'execution'
            if name == 'defaults':
                expected = json.loads(Path(__file__).with_name('tile-defaults.json').read_text())['parameters']
                # New opt-in features retain disabled / single-lane defaults.
                expected['rx_dma_streaming'] = {'category': 'hardware', 'value': False}
                expected['rx_dma_streams'] = {'category': 'hardware', 'value': 1}
                assert settings == expected, {k: (expected.get(k), settings.get(k))
                                              for k in expected.keys() | settings.keys()
                                              if expected.get(k) != settings.get(k)}
        print(f'{name}: PASS', flush=True)
    invalid_resources = [
        ('router-rx-lanes', 'mittens.wormholeRouter', {'id':0,'mesh_width':1,'mesh_height':1,'rx_streams':3}, 'rx_streams must be 1, 2, or 4'),
        ('nic-rx-lanes', 'mittens.wormholeNIC', {'rx_streams':3}, 'rx_streams must be 1, 2, or 4'),
        ('router-mesh', 'mittens.wormholeRouter', {'id': 0, 'mesh_width': 0, 'mesh_height': 1}, 'invalid 0x1 mesh coordinates'),
        ('router-width', 'mittens.wormholeRouter', {'id': 0, 'mesh_width': 1, 'mesh_height': 1, 'link_width_bits': 33}, 'link_width_bits must be'),
        ('router-lanes', 'mittens.wormholeRouter', {'id': 0, 'mesh_width': 1, 'mesh_height': 1, 'tx_streams': 3}, 'requires positive buffer/pipeline sizes'),
        ('router-overflow', 'mittens.wormholeRouter', {'id': 0, 'mesh_width': 4294967295, 'mesh_height': 4294967295}, 'invalid 4294967295x4294967295 mesh coordinates'),
        ('nic-overflow', 'mittens.wormholeNIC', {'mesh_width': 4294967295, 'mesh_height': 4294967295}, 'requires tx_streams in {1,2,4}'),
        ('ram-channels', 'mittens.globalRAMController', {'channels': 0}, 'invalid global RAM configuration'),
        ('ram-reserved', 'mittens.globalRAMController', {'channels': 1, 'reserved_read_channels': 1}, 'must leave at least one write channel'),
        ('ram-mode', 'mittens.globalRAMController', {'dependency_mode': 'unknown'}, 'invalid global RAM dependency_mode'),
        ('ram-duplicates', 'mittens.globalRAMController', {'active_tiles': [0, 0]}, 'contains a duplicate'),
        ('ram-range', 'mittens.globalRAMController', {'active_tiles': [1]}, 'outside tile_count'),
    ]
    for name, resource, params, expected in invalid_resources:
        trial = output / name
        trial.mkdir()
        path = trial / 'input.json'
        path.write_text(json.dumps(params))
        env = dict(environment, MITTENS_CONFIGURATION_INPUT=str(path),
                   MITTENS_CONFIGURATION_RESOURCE=resource,
                   GOLEM_RESOLVED_CONFIG_DIR=str(trial / 'resolved'),
                   SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'))
        with (trial / 'simulation.log').open('w') as log:
            process = subprocess.run([str(install / 'sst-core/bin/sst'),
                                      str(Path(__file__).with_name('configuration.py'))],
                                     env=env, stdout=log, stderr=subprocess.STDOUT, timeout=20)
        assert process.returncode != 0, name
        assert expected in (trial / 'simulation.log').read_text(), (name, trial)
        print(f'{name}: PASS', flush=True)
    positive_resources = [
        ('nic-router-R2', 'mittens.wormholeNIC', {'rx_streams':2}),
        ('nic-router-R4', 'mittens.wormholeNIC', {'rx_streams':4}),
        ('nic-router-defaults', 'mittens.wormholeNIC', {}),
        ('nic-router-T4-wide', 'mittens.wormholeNIC', {'tx_streams': 4, 'link_width_bits': 128,
                                                     'injection_buffer_flits': 128}),
        ('ram-defaults-exact', 'mittens.globalRAMController',
         {'tile_count': 2, 'dependency_mode': 'exact_dependencies'}),
    ]
    for name, resource, params in positive_resources:
        trial = output / name
        trial.mkdir()
        path = trial / 'input.json'
        path.write_text(json.dumps(params))
        env = dict(environment, MITTENS_CONFIGURATION_INPUT=str(path),
                   MITTENS_CONFIGURATION_RESOURCE=resource,
                   MITTENS_CONFIGURATION_POSITIVE='1',
                   GOLEM_RESOLVED_CONFIG_DIR=str(trial / 'resolved'),
                   SST_LIB_PATH=str(install / 'sst-elements/lib/sst-elements-library'))
        with (trial / 'simulation.log').open('w') as log:
            process = subprocess.run([str(install / 'sst-core/bin/sst'),
                                      str(Path(__file__).with_name('configuration.py'))],
                                     env=env, stdout=log, stderr=subprocess.STDOUT, timeout=20)
        assert process.returncode == 0, (name, trial)
        kinds = ('nic', 'router') if resource == 'mittens.wormholeNIC' else ('global-ram',)
        resolved = {kind: list((trial / 'resolved').glob(f'{kind}-*.json')) for kind in kinds}
        for kind, files in resolved.items():
            assert len(files) == 1, (kind, files)
            settings = json.loads(files[0].read_text())['parameters']
            assert settings['clock']['value'] == '1GHz'
            if kind == 'global-ram':
                expected = {'verbose': 0, 'capacity_bytes': 34359738368, 'tile_count': 2,
                            'channels': 1, 'queue_depth': 16, 'per_tile_queue_depth': 8,
                            'setup_cycles': 8, 'bytes_per_cycle': 32, 'burst_bytes': 64,
                            'fixed_latency_cycles': 2, 'maximum_request_bytes': 4294967295,
                            'demand_write_burst': 0, 'read_priority_burst': 0,
                            'reserved_read_channels': 0, 'progress_snapshot_interval_ms': 10000,
                            'dependency_mode': 'exact_dependencies', 'profile_output_directory': '',
                            'clock': '1GHz', 'active_tiles': [0, 1]}
                assert {k: item['value'] for k, item in settings.items()} == expected
            else:
                assert settings['tx_streams']['value'] == params.get('tx_streams', 1)
                assert settings['link_width_bits']['value'] == params.get('link_width_bits', 32)
            if kind == 'nic':
                assert settings['injection_buffer_flits']['value'] == params.get('injection_buffer_flits', 64)
        print(f'{name}: PASS', flush=True)
    print(f'Configuration: {len(cases) + len(invalid_resources) + len(positive_resources)} checks PASS; evidence {output}')


if __name__ == '__main__':
    main()
