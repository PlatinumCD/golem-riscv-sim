"""Whole-tile placement and rejection before backing-file mutation."""
import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE))
from components.mordred.thread_placement import partition_tiles

spec = importlib.util.spec_from_file_location(
    "mesh_fixture", SOURCE / "tests/network-instructions/test_configuration.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class Node(fixture.Node):
    def setRank(self, rank, thread):
        self.placement = (rank, thread)


class Composition(fixture.Composition):
    def __init__(self, threads=4, ranks=1):
        super().__init__()
        self.threads, self.ranks, self.options = threads, ranks, {}

    def getThreadCount(self):
        return self.threads

    def getMPIRankCount(self):
        return self.ranks

    def setProgramOption(self, name, value):
        self.options[name] = value

    def findComponentByName(self, name):
        return self.nodes.get(name)

    def Component(self, name, kind):
        if name in self.nodes:
            raise AssertionError(f"duplicate component: {name}")
        self.nodes[name] = Node(name, kind)
        return self.nodes[name]


class ThreadConfigurationTests(fixture.TileMeshConfigurationTests):
    def test_automatic_partition_matches_experiment_and_covers_rectangular_mesh(self):
        self.assertEqual(partition_tiles(4, 4, 4),
                         (0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3))
        for x, y in ((1, 1), (2, 2), (3, 5), (5, 3), (4, 4), (8, 8)):
            for threads in range(1, min(x * y, 16) + 1):
                with self.subTest(x=x, y=y, threads=threads):
                    mapping = partition_tiles(x, y, threads)
                    self.assertEqual(len(mapping), x * y)
                    self.assertEqual(set(mapping), set(range(threads)))
                    for worker in range(threads):
                        ids = [i for i, w in enumerate(mapping) if w == worker]
                        xs, ys = {i % x for i in ids}, {i // x for i in ids}
                        self.assertEqual(len(ids), len(xs) * len(ys))

    def test_compute_and_dram_components_stay_with_their_router(self):
        image = self.root / "weights.bin"
        image.write_bytes(bytes(1024 * 1024))
        for dram in ({}, {0: dict(image=image, capacity_bytes=1024 * 1024)}):
            graph = Composition()
            result = self.build(graph, dram_tiles=dram)
            self.assertEqual(graph.options["partitioner"], "sst.self")
            self.assertEqual(result["tile_threads"], (0, 1, 2, 3))
            for identity, tile in enumerate(result["tiles"]):
                for node in graph.nodes.values():
                    if node.name.startswith(f"tile_mesh.tile{identity}."):
                        self.assertEqual(node.placement, (0, identity))
                self.assertEqual(tile["cpu"].params["sst_tile_thread"], identity)
                self.assertEqual(result["mesh"]["routers"][identity].placement, (0, identity))
            # Include subcomponents when checking actual link endpoints.
            cross = 0
            for link in graph.links.values():
                left, right = link.ends
                a, b = (graph.nodes[end[0].name.split(":")[0]] for end in (left, right))
                if a.placement != b.placement:
                    self.assertIn(a, result["mesh"]["routers"])
                    self.assertIn(b, result["mesh"]["routers"])
                    self.assertEqual((left[2], right[2]), ("1ns", "1ns"))
                    cross += 1
            self.assertEqual(cross, 4)

    def test_custom_whole_tile_mapping(self):
        graph = Composition(threads=2)
        result = self.build(graph, tile_threads=[1, 0, 1, 0])
        self.assertEqual(result["tile_threads"], (1, 0, 1, 0))
        for tile, worker in zip(result["tiles"], result["tile_threads"]):
            self.assertEqual(tile["cpu"].placement, (0, worker))

    def test_invalid_threading_never_mutates_backing(self):
        self.directory.mkdir()
        sentinel = self.directory / "tile0-spm.bin"
        sentinel.write_bytes(b"keep existing data")
        cases = [(Composition(threads=n), {}) for n in (0, 5, True, 1.5)]
        cases += [(Composition(ranks=2), {})]
        cases += [(Composition(), dict(tile_threads=value)) for value in
                  ([], [0, 1], "0123", [0, 1, 2, -1], [0, 1, 2, 4], [0, 1, 2, True])]
        for graph, arguments in cases:
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                self.build(graph, **arguments)
            self.assertFalse(graph.nodes)
            self.assertFalse(graph.links)
            self.assertEqual(sentinel.read_bytes(), b"keep existing data")
        with patch.dict(os.environ, {"TILE_COMPONENT_TRACE_START_TASK": "0"}):
            graph = Composition()
            with self.assertRaisesRegex(ValueError, "full-lifetime"):
                self.build(graph)
            self.assertFalse(graph.nodes)
            self.assertEqual(sentinel.read_bytes(), b"keep existing data")


if __name__ == "__main__":
    unittest.main()
