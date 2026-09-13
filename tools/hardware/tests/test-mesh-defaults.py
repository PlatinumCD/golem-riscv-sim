"""Check mesh configuration without launching SST or a guest."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest

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
