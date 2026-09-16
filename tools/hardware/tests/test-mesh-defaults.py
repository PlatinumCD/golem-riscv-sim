"""Check mesh configuration without launching SST or a guest."""
import importlib.util
import os
from pathlib import Path
import runpy
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


class Component:
    def __init__(self, name, kind):
        self.name, self.kind, self.params = name, kind, {}
        components[name] = self

    def addParams(self, params):
        self.params.update(params)

    def setSubComponent(self, name, kind):
        return Component(self.name + '.' + name, kind)


class Link:
    def __init__(self, name):
        self.name = name

    def connect(self, *ends):
        pass

    def setNoCut(self):
        pass


components = {}
sys.modules['sst'] = types.SimpleNamespace(
    Component=Component, Link=Link, setStatisticLoadLevel=lambda *a: None,
    setStatisticOutput=lambda *a: None,
    enableAllStatisticsForAllComponents=lambda *a: None)
spec = importlib.util.spec_from_file_location('mesh', ROOT / 'tests/support/mesh.py')
mesh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mesh)


class MeshDefaults(unittest.TestCase):
    def test_registered_mesh_fixture_uses_supported_architecture(self):
        fixtures = {
            'network/mesh-3x3': {
                **{f'MITTENS_MESH_TILE{i}_ELF': f'tile{i}.elf' for i in range(9)},
                'MITTENS_MESH_STATS': 'unused.csv',
            },
        }
        for name, environment in fixtures.items():
            with self.subTest(fixture=name), patch.dict(
                os.environ, {'MITTENS_TEST_QEMU': 'qemu', **environment}, clear=True
            ), patch.dict(sys.modules, {'mesh': mesh, 'support.mesh': mesh}):
                components.clear()
                runpy.run_path(str(ROOT / 'tests' / name / 'simulation.py'))
                tiles = [c for c in components.values() if c.kind == 'mittens.tile']
                self.assertEqual(len(tiles), 2)
                for tile in tiles:
                    self.assertTrue(tile.params['scratchpad_boot'])
                    self.assertTrue(tile.params['scratchpad_enabled'])
                    self.assertEqual(tile.params['memory_backend'], 'streaming')
                self.assertFalse(any('memHierarchy' in c.kind or 'merlin' in c.kind
                                     for c in components.values()))

    def build(self, **overrides):
        components.clear()
        return mesh.build_mesh(width=1, height=1, qemu_path='qemu',
                               images=['tile.elf'], statistics_path='unused.csv',
                               **overrides)

    def test_executable_spm_defaults(self):
        self.build()
        tile = components['tile0'].params
        self.assertIs(tile['scratchpad_boot'], True)
        self.assertIs(tile['scratchpad_enabled'], True)
        self.assertEqual(tile['scratchpad_bytes'], 256 * 1024)
        self.assertEqual(tile['memory_backend'], 'streaming')
        self.assertEqual(components['global_ram'].params['dependency_mode'], 'bulk_barrier')
        self.assertFalse(any('memHierarchy' in c.kind for c in components.values()))

    def test_exact_dependency_controller_uses_the_same_architecture(self):
        self.build(global_memory={"dependency_mode": "exact_dependencies"})
        self.assertEqual(components["global_ram"].params["dependency_mode"], "exact_dependencies")

    def test_explicit_capacity_is_preserved(self):
        self.build(tile_params={'scratchpad_bytes': 32768})
        self.assertEqual(components['tile0'].params['scratchpad_bytes'], 32768)

    def test_incompatible_modes_are_rejected(self):
        for settings in ({'memory_backend': 'native'},
                         {'memory_backend': 'memhierarchy'},
                         {'memory_hierarchy': {}},
                         {'tile_params': {'scratchpad_boot': False}}):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                self.build(**settings)


if __name__ == '__main__':
    unittest.main()
