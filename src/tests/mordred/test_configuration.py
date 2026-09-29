"""Mordred mesh composition checks without loading the SST simulator."""
from dataclasses import asdict, FrozenInstanceError
import importlib.util
from pathlib import Path
import sys
import unittest


SOURCE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("mordred_mesh_configuration", SOURCE / "components/mordred/configuration.py")
CONFIG = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CONFIG
SPEC.loader.exec_module(CONFIG)
MeshParameters, connect_mesh = CONFIG.MeshParameters, CONFIG.connect_mesh


class Node:
    def __init__(self, name, kind):
        self.name, self.kind = name, kind
        self.params, self.subcomponents = {}, {}

    def addParams(self, parameters):
        self.params.update(parameters)

    def setSubComponent(self, slot, kind):
        if slot in self.subcomponents:
            raise AssertionError(f"duplicate subcomponent slot: {self.name}:{slot}")
        node = Node(f"{self.name}:{slot}", kind)
        self.subcomponents[slot] = node
        return node


class Link:
    def __init__(self, name, graph):
        self.name, self.graph = name, graph

    def connect(self, left, right):
        self.ends = (left, right)
        for node, port, latency in self.ends:
            key = (node.name, port)
            if key in self.graph.connected:
                raise AssertionError(f"port connected twice: {key}")
            self.graph.connected[key] = self


class Composition:
    def __init__(self):
        self.nodes, self.links, self.connected = {}, {}, {}

    def Component(self, name, kind):
        if name in self.nodes:
            raise AssertionError(f"duplicate component: {name}")
        node = Node(name, kind)
        self.nodes[name] = node
        return node

    def Link(self, name):
        if name in self.links:
            raise AssertionError(f"duplicate link: {name}")
        link = Link(name, self)
        self.links[name] = link
        return link


def endpoints(count):
    nodes = [Node(f"tile{i}", "application.endpoint") for i in range(count)]
    for i, node in enumerate(nodes):
        node.params.update(id=i, num_peers=count, application_owned="preserve")
    return nodes


class MeshConfigurationTests(unittest.TestCase):
    def test_defaults_and_roundtrip(self):
        p = MeshParameters()
        self.assertEqual((p.x_dim, p.y_dim, p.local_ports, p.num_vns, p.num_vcs), (2, 2, 1, 1, 1))
        self.assertEqual((p.router_count, p.endpoint_count, p.flit_size_bits), (4, 4, 128))
        self.assertEqual(MeshParameters(**asdict(p)), p)
        with self.assertRaises(FrozenInstanceError):
            p.x_dim = 3

    def test_two_by_two_exact_port_mapping(self):
        graph, peers = Composition(), endpoints(4)
        mesh = connect_mesh(graph, endpoints=peers)
        actual = {frozenset((node.name, port) for node, port, _ in link.ends) for link in mesh["router_links"]}
        expected = {
            frozenset((("mesh.router.0.0", "port0"), ("mesh.router.0.1", "port2"))),
            frozenset((("mesh.router.0.0", "port1"), ("mesh.router.1.0", "port3"))),
            frozenset((("mesh.router.1.0", "port0"), ("mesh.router.1.1", "port2"))),
            frozenset((("mesh.router.0.1", "port1"), ("mesh.router.1.1", "port3"))),
        }
        self.assertEqual(actual, expected)
        self.assertEqual(len(mesh["links"]), 8)
        self.assertEqual(tuple(router.params["id"] for router in mesh["routers"]), (0, 1, 2, 3))
        self.assertEqual(mesh["endpoints"], tuple(peers))
        for i, (router, nic, link) in enumerate(zip(mesh["routers"], mesh["nics"], mesh["endpoint_links"])):
            self.assertEqual(link.ends, ((router, "port4", "1ns"), (nic, "port", "1ns")))
            self.assertIs(peers[i].subcomponents["networkIF"], nic)
            self.assertEqual(nic.kind, "mordred.mordredNIC")
            self.assertEqual(peers[i].params, dict(id=i, num_peers=4, application_owned="preserve"))

    def test_boundary_ports_have_no_links(self):
        graph = Composition()
        mesh = connect_mesh(graph, endpoints=endpoints(4))
        forbidden = ((0, "port2"), (0, "port3"), (1, "port1"), (1, "port2"),
                     (2, "port0"), (2, "port3"), (3, "port0"), (3, "port1"))
        for router_id, port in forbidden:
            self.assertNotIn((mesh["routers"][router_id].name, port), graph.connected)

    def test_rectangle_and_concentration_use_row_major_endpoint_ids(self):
        graph, p = Composition(), MeshParameters(x_dim=3, y_dim=2, local_ports=2, num_vcs=2)
        mesh = connect_mesh(graph, p, "fabric", endpoints(12))
        self.assertEqual(len(mesh["router_links"]), 7)
        self.assertEqual(len(mesh["endpoint_links"]), 12)
        expected_routers = ("fabric.router.0.0", "fabric.router.1.0", "fabric.router.2.0",
                            "fabric.router.0.1", "fabric.router.1.1", "fabric.router.2.1")
        self.assertEqual(tuple(node.name for node in mesh["routers"]), expected_routers)
        for i, link in enumerate(mesh["endpoint_links"]):
            self.assertEqual(link.ends[0][0].name, expected_routers[i // 2])
            self.assertEqual(link.ends[0][1], "port4" if i % 2 == 0 else "port5")
            self.assertEqual(link.ends[1][0].name, f"tile{i}:networkIF")
        for router in mesh["routers"]:
            self.assertEqual((router.params["num_ports"], router.params["num_vcs"]), (6, 2))

    def test_single_router_and_single_dimension_meshes(self):
        for x, y, edges in ((1, 1, 0), (1, 3, 2), (4, 1, 3)):
            with self.subTest(x=x, y=y):
                graph = Composition()
                mesh = connect_mesh(graph, dict(x_dim=x, y_dim=y), endpoints=endpoints(x*y))
                self.assertEqual(len(mesh["router_links"]), edges)
                self.assertEqual(len(mesh["endpoint_links"]), x*y)
                if x == 1:
                    self.assertFalse(any(port in ("port1", "port3") for _, port in graph.connected))
                if y == 1:
                    self.assertFalse(any(port in ("port0", "port2") for _, port in graph.connected))

    def test_router_only_composition_keeps_local_ports_unconnected(self):
        graph = Composition()
        mesh = connect_mesh(graph)
        self.assertEqual(len(mesh["routers"]), 4)
        self.assertEqual(len(mesh["links"]), 4)
        self.assertEqual((mesh["nics"], mesh["endpoints"], mesh["endpoint_links"]), ((), (), ()))
        self.assertTrue(all(port in ("port0", "port1", "port2", "port3") for _, port in graph.connected))

    def test_parameters_are_forwarded_in_explicit_bit_units(self):
        graph = Composition()
        p = MeshParameters(x_dim=1, y_dim=1, num_vcs=2, clock=" 500 MHz ", link_latency=" 0.8 ns ",
            flit_size_bits=64, router_input_buffer_flits=2, router_output_buffer_flits=3,
            nic_input_buffer_bytes=256, nic_output_buffer_bytes=128, verbose=1)
        mesh = connect_mesh(graph, p, endpoints=endpoints(1))
        self.assertEqual(mesh["routers"][0].params, dict(id=0, clock="500MHz", num_ports=5,
            num_local_ports=1, num_vns=1, num_vcs=2, flit_size="64b", input_buf_size="128b",
            output_buf_size="192b", verbose=1))
        self.assertEqual(mesh["topologies"][0].params, dict(xDim=1, yDim=1, verbose=1))
        self.assertEqual(mesh["nics"][0].params, dict(clock="500MHz", input_buf_size="2048b",
            output_buf_size="1024b", verbose=1))
        self.assertEqual(tuple(end[2] for end in mesh["links"][0].ends), ("0.8ns", "0.8ns"))

    def test_mesh_names_isolate_compositions(self):
        graph = Composition()
        first = connect_mesh(graph, name="first")
        second = connect_mesh(graph, name="second")
        self.assertEqual(len(graph.nodes), 8)
        self.assertEqual(len(graph.links), 8)
        self.assertFalse(set(first["routers"]) & set(second["routers"]))

    def test_integer_fields_reject_nonpositive_nonintegral_and_nonfinite(self):
        fields = ("x_dim", "y_dim", "local_ports", "num_vns", "num_vcs", "flit_size_bits",
                  "router_input_buffer_flits", "router_output_buffer_flits",
                  "nic_input_buffer_bytes", "nic_output_buffer_bytes")
        for field in fields:
            for value in (0, -1, True, 1.0, "1", None, float("inf"), float("nan")):
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, field):
                    MeshParameters(**{field: value})

    def test_one_virtual_network_scope_is_explicit(self):
        with self.assertRaisesRegex(ValueError, "num_vns"):
            MeshParameters(num_vns=2)

    def test_buffer_alignment_and_credit_limits(self):
        invalid = (dict(flit_size_bits=127), dict(nic_input_buffer_bytes=15),
                   dict(nic_output_buffer_bytes=17), dict(nic_output_buffer_bytes=16),
                   dict(router_output_buffer_flits=32768),
                   dict(router_input_buffer_flits=1 << 31),
                   dict(router_input_buffer_flits=1 << 25),
                   dict(nic_input_buffer_bytes=(1 << 31)*16),
                   dict(flit_size_bits=1 << 32), dict(num_vcs=1 << 32))
        for params in invalid:
            with self.subTest(params=params), self.assertRaises(ValueError):
                MeshParameters(**params)
        self.assertEqual(MeshParameters(router_output_buffer_flits=32767).router_output_buffer_flits, 32767)

    def test_endpoint_id_overflow_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "endpoint IDs"):
            MeshParameters(x_dim=65536, y_dim=65536)
        with self.assertRaisesRegex(ValueError, "endpoint IDs"):
            MeshParameters(x_dim=65536, y_dim=16384, local_ports=3)

    def test_invalid_timing_rejected_before_sst_mutation(self):
        for field in ("clock", "link_latency"):
            values = ("", "0GHz", "-1ns", "NaNGHz", "infns", "1e999GHz", "1e-999ns", "1B", 1, None)
            for value in values:
                graph = Composition()
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, field):
                    connect_mesh(graph, {field: value})
                self.assertFalse(graph.nodes)
                self.assertFalse(graph.links)

    def test_invalid_endpoint_lists_rejected_before_topology_mutation(self):
        shared = Node("shared", "application.endpoint")
        for peers in ([], endpoints(3), endpoints(5), [shared]*4, [None]*4,
                      [object(), *endpoints(3)], "abcd", iter(endpoints(4))):
            graph = Composition()
            with self.subTest(peers=peers), self.assertRaises(ValueError):
                connect_mesh(graph, endpoints=peers)
            self.assertFalse(graph.nodes)
            self.assertFalse(graph.links)
            self.assertFalse(shared.subcomponents)

    def test_invalid_names_and_parameter_fields_rejected_before_mutation(self):
        for name in ("", "bad name", "\tmesh", None, 1):
            graph = Composition()
            with self.subTest(name=name), self.assertRaises(ValueError):
                connect_mesh(graph, name=name)
            self.assertFalse(graph.nodes)
        for params in (dict(unknown=1), [], "mesh"):
            graph = Composition()
            with self.subTest(params=params), self.assertRaises(ValueError):
                connect_mesh(graph, params)
            self.assertFalse(graph.nodes)


if __name__ == "__main__":
    unittest.main()
