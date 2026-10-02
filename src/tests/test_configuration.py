"""Shared VLEN, derived register-link width, and composition regressions.

Run with: python3 -B source_new/tests/test_configuration.py
Actual C++ parameter rejection and link timing are covered by tests/run.py.
"""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from configuration import (connect_arrays, connect_scratchpad, _connect_scratchpad,
                           connect_riscv, connect_riscv_arrays, resolve)


class Node:
    def __init__(self, name, kind):
        self.name, self.kind, self.params, self.subcomponents = name, kind, {}, {}

    def addParams(self, params):
        self.params.update(params)

    def addParam(self, name, value):
        self.params[name] = value

    def setSubComponent(self, name, kind):
        node = Node(f"{self.name}:{name}", kind)
        self.subcomponents[name] = node
        return node


class Link:
    def connect(self, left, right):
        self.ends = left, right


class Composition:
    """Capture the public SST configuration calls without loading SST/QEMU."""
    def __init__(self):
        self.nodes, self.links = {}, {}

    def Component(self, name, kind):
        node = Node(name, kind)
        self.nodes[name] = node
        return node

    def Link(self, name):
        link = Link()
        self.links[name] = link
        return link


class ConfigurationTests(unittest.TestCase):
    def test_bank_maps_resolve_to_real_ids_and_preserve_snapshots(self):
        defaults = resolve()
        self.assertEqual(defaults["spm_banks"], 8)
        self.assertEqual(defaults["cpu_spm_banks"], list(range(8)))
        self.assertEqual(defaults["router_spm_banks"], [])
        self.assertEqual(resolve(defaults), defaults)
        parameters = dict(spm_banks=4, cpu_spm_banks=[0, 1, 2, 3], router_spm_banks=(2, 3))
        actual = resolve(parameters)
        self.assertEqual(actual["cpu_spm_banks"], [0, 1, 2, 3])
        self.assertEqual(actual["router_spm_banks"], [2, 3])
        actual["cpu_spm_banks"].clear()
        self.assertEqual(parameters["cpu_spm_banks"], [0, 1, 2, 3])
        self.assertEqual(resolve(dict(spm_banks=2))["cpu_spm_banks"], [0, 1])

    def test_invalid_physical_bank_ids_are_rejected(self):
        for field in ("cpu_spm_banks", "router_spm_banks"):
            for value in (1, True, "0,1", {0, 1}, [-1], [4], [0, 0], [True], [1.0], ["1"]):
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, field):
                    resolve(dict(spm_banks=4, **{field: value}))

    def test_bank_access_settings_reach_controller_and_backend_only(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            graph = Composition()
            peer = graph.Component("peer", "fixture")
            cpu, arrays, scratch = connect_riscv_arrays(graph,
                dict(spm_banks=4, cpu_spm_banks=[0, 2, 3], router_spm_banks=[2, 3]),
                elf=elf, qemu=qemu, memory_file=directory / "spm.bin", name_prefix="tile.",
                clients=[peer], router_requestor="tile.router_spm:memory")
            expected = dict(cpu_spm_banks=[0, 2, 3], router_spm_banks=[2, 3],
                            cpu_requestor="tile.riscv:qemu_memory", router_requestor="tile.router_spm:memory")
            backend = scratch.subcomponents["backendConvertor"].subcomponents["backend"]
            for node in (scratch, backend):
                self.assertEqual({key: node.params[key] for key in expected}, expected)
                self.assertEqual(node.params["spm_banks"], 4)
                self.assertEqual(node.params["spm_bank_width"], 4)
            for node in (cpu, arrays):
                self.assertFalse(expected.keys() & node.params.keys())

    def test_unbound_memory_fixtures_preserve_generic_client_access(self):
        graph = Composition()
        peer = graph.Component("peer", "fixture")
        scratch = connect_scratchpad(graph, {}, clients=[peer])
        backend = scratch.subcomponents["backendConvertor"].subcomponents["backend"]
        for node in (scratch, backend):
            self.assertEqual(node.params["cpu_requestor"], "")
            self.assertEqual(node.params["router_requestor"], "")
            self.assertEqual(node.params["cpu_spm_banks"], list(range(8)))
            self.assertEqual(node.params["router_spm_banks"], [])

    def test_ordinary_memory_peers_bind_roles_without_external_commit_owner(self):
        graph = Composition()
        cpu = graph.Component("driver", "fixture").setSubComponent("cpu", "memory")
        router = graph.Component("router", "fixture").setSubComponent("memory", "memory")
        scratch = connect_scratchpad(graph,
            dict(spm_banks=4, cpu_spm_banks=[0, 2], router_spm_banks=[1, 2]),
            clients=[cpu, router], cpu_requestor="driver:cpu", router_requestor="router:memory")
        backend = scratch.subcomponents["backendConvertor"].subcomponents["backend"]
        for node in (scratch, backend):
            self.assertEqual(node.params["cpu_requestor"], "driver:cpu")
            self.assertEqual(node.params["router_requestor"], "router:memory")
            self.assertEqual(node.params["cpu_spm_banks"], [0, 2])
            self.assertEqual(node.params["router_spm_banks"], [1, 2])
        self.assertNotIn("external_write_requestor", scratch.params)

    def test_conflicting_or_empty_role_bindings_fail_before_topology_creation(self):
        p = resolve(dict(router_spm_banks=[2, 3]))
        invalid = (dict(cpu_requestor="cpu", external_write_requestor="other"),
                   dict(cpu_requestor="same", router_requestor="same"),
                   dict(cpu_requestor="bad name"), dict(router_requestor=3))
        for options in invalid:
            graph = Composition()
            with self.subTest(options=options), self.assertRaises(ValueError):
                _connect_scratchpad(graph, p, (), **options)
            self.assertFalse(graph.nodes)
        for field, requestor in (("cpu_spm_banks", "cpu_requestor"), ("router_spm_banks", "router_requestor")):
            graph = Composition()
            with self.subTest(field=field), self.assertRaises(ValueError):
                connect_scratchpad(graph, {field: []}, **{requestor: "named:memory"})
            self.assertFalse(graph.nodes)

    def test_real_cpu_requires_nonempty_bank_list_before_any_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for compose in (connect_riscv, connect_riscv_arrays):
                graph = Composition()
                backing.write_bytes(b"preserve")
                with self.subTest(compose=compose.__name__), self.assertRaisesRegex(ValueError, "cpu_spm_banks"):
                    compose(graph, dict(cpu_spm_banks=[]), elf=elf, qemu=qemu, memory_file=backing)
                self.assertFalse(graph.nodes)
                self.assertEqual(backing.read_bytes(), b"preserve")

    def test_array_program_delay_scope_default_and_forwarding(self):
        self.assertEqual(resolve()["array_program_delay_scope"], "per_command")
        for scope in ("per_command", "initial_full_array"):
            with self.subTest(scope=scope):
                parameters = resolve(dict(array_program_delay_scope=scope, cost_per_array_program_cycles=65536))
                self.assertEqual(resolve(parameters), parameters)
                arrays = connect_arrays(Composition(), parameters)
                self.assertEqual(arrays.params["array_program_delay_scope"], scope)
                self.assertEqual(arrays.params["cost_per_array_program_cycles"], 65536)

    def test_invalid_program_delay_scope_fails_before_side_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            backing = directory / "scratchpad.bin"
            for value in (None, False, 0, "whole_array", "", ["per_command"]):
                composition = Composition()
                with self.subTest(scope=value), self.assertRaisesRegex(ValueError, "array_program_delay_scope"):
                    connect_riscv_arrays(composition, dict(array_program_delay_scope=value),
                        elf=directory / "missing.elf", qemu=directory / "missing-qemu", memory_file=backing)
                self.assertFalse(composition.nodes)
                self.assertFalse(backing.exists())

    def test_widths_and_snapshot_roundtrip(self):
        for vlen, width in ((128, 16), (256, 32), (512, 64), (1024, 128)):
            with self.subTest(vlen=vlen):
                parameters = resolve(dict(riscv_vector_length_bits=vlen))
                self.assertEqual(parameters["array_link_width"], width)
                self.assertEqual(resolve(parameters), parameters)
                self.assertEqual(resolve(dict(riscv_vector_length_bits=vlen, array_link_width=width)), parameters)

    def test_conflicting_width_is_rejected(self):
        for vlen in (128, 256, 512, 1024):
            for width in (0, 1, vlen // 8 + 1, True, float(vlen // 8)):
                with self.subTest(vlen=vlen, width=width), self.assertRaises(ValueError):
                    resolve(dict(riscv_vector_length_bits=vlen, array_link_width=width))

    def test_legacy_cpu_spelling_resolves_the_same_width(self):
        for vlen in (128, 256, 512, 1024):
            with self.subTest(vlen=vlen):
                legacy = dict(riscv_vector_length_bits=vlen)
                expected = resolve(legacy)
                self.assertEqual(resolve(cpu_parameters=legacy), expected)
                self.assertEqual(resolve(expected, cpu_parameters=legacy), expected)
                self.assertEqual(resolve(dict(array_link_width=vlen // 8), cpu_parameters=legacy), expected)

    def test_cpu_architecture_conflicts_are_rejected(self):
        for architecture, cpu in ((128, 256), (256, 512), (1024, 128)):
            with self.subTest(architecture=architecture, cpu=cpu), self.assertRaises(ValueError):
                resolve(dict(riscv_vector_length_bits=architecture),
                        cpu_parameters=dict(riscv_vector_length_bits=cpu))
        with self.assertRaises(ValueError):
            resolve(dict(array_link_width=32), cpu_parameters=dict(riscv_vector_length_bits=512))

    def test_invalid_vlen_is_rejected_through_both_spellings(self):
        for value in (0, 64, 192, 2048, True, 256.0, "256"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError): resolve(dict(riscv_vector_length_bits=value))
                with self.assertRaises(ValueError): resolve(cpu_parameters=dict(riscv_vector_length_bits=value))

    def test_other_cpu_controls_do_not_change_link_width(self):
        parameters = resolve(cpu_parameters=dict(instruction_budget=1, issue_width=4,
                                                 riscv_vector_element_bits=32))
        self.assertEqual(parameters["riscv_vector_length_bits"], 256)
        self.assertEqual(parameters["array_link_width"], 32)

    def test_standalone_arrays_receive_shared_vlen_and_derived_width(self):
        for vlen in (128, 256, 512, 1024):
            with self.subTest(vlen=vlen):
                arrays = connect_arrays(Composition(), dict(riscv_vector_length_bits=vlen))
                self.assertEqual(arrays.params["riscv_vector_length_bits"], vlen)
                self.assertEqual(arrays.params["array_link_width"], vlen // 8)
                self.assertFalse(arrays.subcomponents)
                self.assertFalse(any(key.startswith("spm_") for key in arrays.params))

    def test_cpu_and_array_composition_share_vlen(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for vlen in (128, 256, 512, 1024):
                for legacy in (False, True):
                    with self.subTest(vlen=vlen, legacy=legacy):
                        architecture = {} if legacy else dict(riscv_vector_length_bits=vlen)
                        cpu_options = dict(riscv_vector_length_bits=vlen) if legacy else {}
                        composition = Composition()
                        cpu, arrays, _ = connect_riscv_arrays(composition, architecture,
                            elf=elf, qemu=qemu, memory_file=directory / "spm.bin", cpu_parameters=cpu_options)
                        self.assertEqual(cpu.params["riscv_vector_length_bits"], vlen)
                        self.assertEqual(arrays.params["riscv_vector_length_bits"], vlen)
                        self.assertEqual(arrays.params["array_link_width"], vlen // 8)
                        self.assertFalse(arrays.subcomponents)
                        array_links = [link for link in composition.links.values()
                                       if any(end[0] is arrays for end in link.ends)]
                        self.assertEqual(len(array_links), 1)
                        self.assertEqual(array_links[0].ends[0][1], "analog_commands")
                        self.assertEqual(array_links[0].ends[1][1], "commands")

    def test_standalone_cpu_uses_architecture_vlen(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            cpu, _ = connect_riscv(Composition(), dict(riscv_vector_length_bits=1024),
                                  elf=elf, qemu=qemu, memory_file=directory / "spm.bin")
            self.assertEqual(cpu.params["riscv_vector_length_bits"], 1024)

    def test_spm_default_capacity_remains_two_mib(self):
        capacity = 2 * 1024 * 1024
        self.assertEqual(resolve()["spm_capacity_bytes"], capacity)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                with self.subTest(connect=connect.__name__):
                    components = connect(Composition(), {}, elf=elf, qemu=qemu, memory_file=backing)
                    self.assertEqual(components[0].params["spm_capacity_bytes"], capacity)
                    self.assertEqual(components[-1].params["size"], f"{capacity}B")
                    self.assertEqual(backing.stat().st_size, capacity)

    def test_spm_thirty_two_mib_capacity_is_forwarded_and_backed(self):
        capacity = 32 * 1024 * 1024
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                with self.subTest(connect=connect.__name__):
                    components = connect(Composition(), dict(spm_capacity_bytes=capacity),
                                         elf=elf, qemu=qemu, memory_file=backing)
                    self.assertEqual(components[0].params["spm_capacity_bytes"], capacity)
                    self.assertEqual(components[-1].params["size"], f"{capacity}B")
                    self.assertEqual(backing.stat().st_size, capacity)

    def test_spm_above_thirty_two_mib_fails_before_topology_or_backing_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            # Both sizes satisfy page/request alignment: only the capacity ceiling rejects them.
            for capacity in (32 * 1024 * 1024 + 4096, 64 * 1024 * 1024):
                for connect in (connect_riscv, connect_riscv_arrays):
                    with self.subTest(connect=connect.__name__, capacity=capacity):
                        composition = Composition()
                        backing.write_bytes(b"preserve existing backing")
                        with self.assertRaisesRegex(ValueError, "QEMU SPM capacity must be at most 32 MiB"):
                            connect(composition, dict(spm_capacity_bytes=capacity),
                                    elf=elf, qemu=qemu, memory_file=backing)
                        self.assertFalse(composition.nodes)
                        self.assertEqual(backing.read_bytes(), b"preserve existing backing")
                        backing.unlink()
                        with self.assertRaisesRegex(ValueError, "QEMU SPM capacity must be at most 32 MiB"):
                            connect(composition, dict(spm_capacity_bytes=capacity),
                                    elf=elf, qemu=qemu, memory_file=backing)
                        self.assertFalse(composition.nodes)
                        self.assertFalse(backing.exists())

    def test_pipeline_mode_is_shared_by_cpu_and_array(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for parameters, enabled in (({}, True), (dict(array_pipeline_enabled=False), False),
                                        (dict(array_pipeline_enabled=True), True)):
                with self.subTest(parameters=parameters):
                    cpu, arrays, _ = connect_riscv_arrays(Composition(),
                        parameters, elf=elf, qemu=qemu,
                        memory_file=directory / "spm.bin")
                    self.assertIs(cpu.params["array_pipeline_enabled"], enabled)
                    self.assertIs(arrays.params["array_pipeline_enabled"], enabled)
                    self.assertFalse(arrays.subcomponents)

    def test_invalid_pipeline_mode_fails_before_creating_backing(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            backing = directory / "spm.bin"
            for value in (0, 1, "true", None):
                composition = Composition()
                with self.subTest(value=value), self.assertRaises(ValueError):
                    connect_riscv_arrays(composition, dict(array_pipeline_enabled=value),
                        elf=directory / "missing.elf", qemu=directory / "missing-qemu",
                        memory_file=backing)
                self.assertFalse(composition.nodes)
                self.assertFalse(backing.exists())

    def test_conflicts_fail_before_creating_components_or_backing(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            backing = directory / "spm.bin"
            for connect in (connect_riscv, connect_riscv_arrays):
                composition = Composition()
                with self.subTest(connect=connect.__name__), self.assertRaises(ValueError):
                    connect(composition, dict(riscv_vector_length_bits=128),
                            cpu_parameters=dict(riscv_vector_length_bits=512),
                            elf=directory / "missing.elf", qemu=directory / "missing-qemu", memory_file=backing)
                self.assertFalse(composition.nodes)
                self.assertFalse(backing.exists())

    def test_instruction_cache_defaults(self):
        expected = dict(instruction_cache_enabled=True, instruction_cache_bytes=8192,
                        instruction_cache_line_bytes=64, instruction_cache_ways=2,
                        instruction_cache_hit_cycles=1)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                with self.subTest(connect=connect.__name__):
                    cpu = connect(Composition(), {}, elf=elf, qemu=qemu,
                                  memory_file=directory / "spm.bin")[0]
                    self.assertEqual({key: cpu.params[key] for key in expected}, expected)

    def test_load_store_queue_default_and_custom_depths(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for depth in (None, 1, 2, 4, 8, 16, 64):
                    options = {} if depth is None else dict(load_store_queue_depth=depth)
                    with self.subTest(connect=connect.__name__, depth=depth):
                        nodes = connect(Composition(), {}, elf=elf, qemu=qemu,
                            memory_file=directory / "spm.bin", cpu_parameters=options)
                        self.assertEqual(nodes[0].params["load_store_queue_depth"], depth or 1)
                        for node in nodes[1:]:
                            self.assertNotIn("load_store_queue_depth", node.params)

    def test_invalid_load_store_queue_depth_preserves_existing_backing(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for depth in (0, -1, 65, True, False, 1.0, "4", None):
                    with self.subTest(connect=connect.__name__, depth=depth):
                        composition = Composition()
                        backing.write_bytes(b"preserve existing backing")
                        with self.assertRaisesRegex(ValueError, "load_store_queue_depth"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=dict(load_store_queue_depth=depth))
                        self.assertFalse(composition.nodes)
                        self.assertEqual(backing.read_bytes(), b"preserve existing backing")
                        backing.unlink()
                        with self.assertRaisesRegex(ValueError, "load_store_queue_depth"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=dict(load_store_queue_depth=depth))
                        self.assertFalse(backing.exists())

    def test_scalar_queue_default_disabled_and_custom_depths(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for depth in (None, 0, 1, 2, 4, 8, 16, 64):
                    options = {} if depth is None else dict(scalar_load_store_queue_depth=depth)
                    with self.subTest(connect=connect.__name__, depth=depth):
                        nodes = connect(Composition(), {}, elf=elf, qemu=qemu,
                            memory_file=directory / "spm.bin", cpu_parameters=options)
                        self.assertEqual(nodes[0].params["scalar_load_store_queue_depth"],
                                         8 if depth is None else depth)
                        for node in nodes[1:]:
                            self.assertNotIn("scalar_load_store_queue_depth", node.params)

    def test_invalid_scalar_queue_depth_preserves_backing(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for depth in (-1, 65, True, False, 1.0, "8", None):
                    with self.subTest(connect=connect.__name__, depth=depth):
                        composition = Composition()
                        backing.write_bytes(b"preserve existing backing")
                        with self.assertRaisesRegex(ValueError, "scalar_load_store_queue_depth"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=dict(scalar_load_store_queue_depth=depth))
                        self.assertFalse(composition.nodes)
                        self.assertEqual(backing.read_bytes(), b"preserve existing backing")
                        backing.unlink()
                        with self.assertRaisesRegex(ValueError, "scalar_load_store_queue_depth"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=dict(scalar_load_store_queue_depth=depth))
                        self.assertFalse(backing.exists())

    def test_tile_profiles_inherit_scalar_queue(self):
        import json
        from configuration import _cpu_options
        profiles = json.loads((Path(__file__).resolve().parents[1] / "tile_profiles.json").read_text())
        for name, profile in profiles.items():
            with self.subTest(profile=name):
                options = _cpu_options(resolve(profile["parameters"]), profile["cpu_parameters"])
                self.assertEqual(options["scalar_load_store_queue_depth"], 8)

    def test_instruction_cache_custom_and_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for options in (dict(instruction_cache_bytes=16384, instruction_cache_line_bytes=128,
                                 instruction_cache_ways=4, instruction_cache_hit_cycles=3),
                            dict(instruction_cache_enabled=False)):
                for connect in (connect_riscv, connect_riscv_arrays):
                    with self.subTest(connect=connect.__name__, options=options):
                        cpu = connect(Composition(), {}, elf=elf, qemu=qemu,
                                      memory_file=directory / "spm.bin", cpu_parameters=options)[0]
                        self.assertEqual({key: cpu.params[key] for key in options}, options)

    def test_invalid_cache_settings_fail_before_topology_or_backing_changes(self):
        invalid = [
            (dict(instruction_cache_enabled=value), {}) for value in (0, 1, "false")
        ] + [
            (dict(instruction_cache_bytes=value), {}) for value in (0, 3, True, 32*1024*1024, 64)
        ] + [
            (dict(instruction_cache_line_bytes=value), {}) for value in (0, 2, 48, True, 4*1024*1024)
        ] + [
            (dict(instruction_cache_ways=value), {}) for value in (0, 3, True, 256)
        ] + [
            (dict(instruction_cache_hit_cycles=value), {}) for value in (0, -1, True, 1.0, 2**31)
        ] + [
            (dict(instruction_cache_line_bytes=8192, instruction_cache_ways=1), dict(spm_capacity_bytes=12288)),
            (dict(instruction_cache_enabled=False, instruction_cache_hit_cycles=0), {}),
        ]
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for options, architecture in invalid:
                for connect in (connect_riscv, connect_riscv_arrays):
                    with self.subTest(connect=connect.__name__, options=options, architecture=architecture):
                        composition = Composition()
                        backing.write_bytes(b"preserve existing backing")
                        with self.assertRaisesRegex(ValueError, "instruction_cache"):
                            connect(composition, architecture, elf=elf, qemu=qemu,
                                    memory_file=backing, cpu_parameters=options)
                        self.assertFalse(composition.nodes)
                        self.assertEqual(backing.read_bytes(), b"preserve existing backing")
                        backing.unlink()
                        with self.assertRaisesRegex(ValueError, "instruction_cache"):
                            connect(composition, architecture, elf=elf, qemu=qemu,
                                    memory_file=backing, cpu_parameters=options)
                        self.assertFalse(composition.nodes)
                        self.assertFalse(backing.exists())


    def test_analog_queue_controls_are_cpu_only(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu = directory / "guest.elf", directory / "qemu"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for options in ({}, dict(analog_command_queue_depth=0),
                                dict(analog_command_queue_depth=1, analog_command_queue_bytes=1024),
                                dict(analog_command_queue_depth=4),
                                dict(analog_command_queue_depth=16, analog_command_queue_bytes=16384)):
                    with self.subTest(connect=connect.__name__, options=options):
                        nodes = connect(Composition(), {}, elf=elf, qemu=qemu,
                            memory_file=directory / "spm.bin", cpu_parameters=options)
                        self.assertEqual(nodes[0].params["analog_command_queue_depth"],
                                         options.get("analog_command_queue_depth", 4))
                        self.assertEqual(nodes[0].params["analog_command_queue_bytes"],
                                         options.get("analog_command_queue_bytes", 16384))
                        for node in nodes[1:]:
                            self.assertNotIn("analog_command_queue_depth", node.params)
                            self.assertNotIn("analog_command_queue_bytes", node.params)

    def test_invalid_analog_queue_controls_preserve_backing(self):
        invalid = [dict(analog_command_queue_depth=v) for v in (-1, 17, True, 1.0, "4", None)]
        invalid += [dict(analog_command_queue_bytes=v) for v in (0, 1020, 1025, 16388, True, 1024.0, "1024", None)]
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            elf, qemu, backing = directory / "guest.elf", directory / "qemu", directory / "spm.bin"
            elf.touch(); qemu.touch()
            for connect in (connect_riscv, connect_riscv_arrays):
                for options in invalid:
                    with self.subTest(connect=connect.__name__, options=options):
                        composition = Composition()
                        backing.write_bytes(b"preserve existing backing")
                        with self.assertRaisesRegex(ValueError, "analog_command_queue"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=options)
                        self.assertFalse(composition.nodes)
                        self.assertEqual(backing.read_bytes(), b"preserve existing backing")
                        backing.unlink()
                        with self.assertRaisesRegex(ValueError, "analog_command_queue"):
                            connect(composition, {}, elf=elf, qemu=qemu, memory_file=backing,
                                    cpu_parameters=options)
                        self.assertFalse(backing.exists())


if __name__ == "__main__": unittest.main(verbosity=2)
