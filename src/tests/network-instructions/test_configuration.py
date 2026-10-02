"""Tile mesh wiring and input-preservation tests; no SST installation required."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from configuration import (connect, connect_arrays, connect_scratchpad,
                           _connect_scratchpad, connect_riscv, connect_riscv_arrays,
                           resolve)
from components.mordred.configuration import MeshParameters
from components.mordred.tiles import connect_riscv_mesh


class Node:
    def __init__(self, name, kind):
        self.name, self.kind, self.params, self.subcomponents = name, kind, {}, {}

    def addParams(self, params):
        self.params.update(params)

    def addParam(self, name, value):
        self.params[name] = value

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
        for node, port, _ in self.ends:
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


class TileMeshConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.qemu = self.root / "qemu"
        self.qemu.write_bytes(b"QEMU input must survive")
        self.elfs = []
        for i in range(4):
            elf = self.root / f"guest{i}.elf"
            elf.write_bytes(f"guest ELF {i} must survive".encode())
            self.elfs.append(elf)
        self.directory = self.root / "memory"
        self.inputs = {p: p.read_bytes() for p in [self.qemu, *self.elfs]}

    def build(self, graph=None, **overrides):
        arguments = dict(parameters={}, elfs=self.elfs, memory_directory=self.directory,
                         qemu=self.qemu)
        arguments.update(overrides)
        return connect_riscv_mesh(graph if graph is not None else Composition(), **arguments)

    def assert_rejected_without_mutation(self, **overrides):
        graph = Composition()
        self.directory.mkdir(exist_ok=True)
        sentinel = self.directory / "tile0-spm.bin"
        sentinel.write_bytes(b"existing backing must survive")
        before = {p: p.read_bytes() for p in self.directory.iterdir() if p.is_file()}
        with self.assertRaises(ValueError):
            self.build(graph, **overrides)
        self.assertEqual((graph.nodes, graph.links), ({}, {}))
        self.assertEqual({p: p.read_bytes() for p in before}, before)
        self.assertEqual({p: p.read_bytes() for p in self.inputs}, self.inputs)

    def test_tile_profiles_have_exact_capacity_and_receive_bounds(self):
        profiles = json.loads((Path(__file__).resolve().parents[2] / 'tile_profiles.json').read_text())
        for name, capacity, banks in (('small', 1048576, 2), ('medium', 1572864, 4), ('large', 2097152, 8)):
            with self.subTest(profile=name):
                profile = profiles[name]
                self.assertEqual(profile['parameters']['spm_capacity_bytes'], capacity)
                self.assertEqual(profile['parameters']['spm_banks'], banks)
                transfer = dict(transfer_id=7, source_tile=0, destination_tile=3,
                                receive_base=0x90000000+capacity-512, slot_capacity=256, slot_count=2)
                graph = Composition()
                result = self.build(graph, **profile, network_transfers=[transfer])
                for i, tile in enumerate(result['tiles']):
                    self.assertEqual(tile['cpu'].params['spm_capacity_bytes'], capacity)
                    self.assertEqual(tile['scratchpad'].params['size'], f'{capacity}B')
                    self.assertEqual(tile['router_spm'].params['spm_capacity_bytes'], capacity)
                    self.assertEqual((self.directory / f'tile{i}-spm.bin').stat().st_size, capacity)
                    self.assertEqual(tile['scratchpad'].params['cpu_spm_banks'], list(range(banks)))
                # A full final slot is valid; one more slot must fail before mutation.
                self.assert_rejected_without_mutation(**profile,
                    network_transfers=[transfer | dict(slot_count=3)])

    def test_four_complete_tiles_have_exact_local_wiring_and_separate_backing(self):
        graph = Composition()
        result = self.build(graph)
        self.assertEqual((len(graph.nodes), len(graph.links)), (24, 32))
        self.assertEqual(len(result["tiles"]), 4)
        self.assertEqual(len(result["mesh"]["routers"]), 4)
        self.assertEqual(len(result["mesh"]["router_links"]), 4)
        paths = set()
        for i, tile in enumerate(result["tiles"]):
            prefix = f"tile_mesh.tile{i}."
            cpu, arrays, scratch, router_spm = (tile[k] for k in ("cpu", "arrays", "scratchpad", "router_spm"))
            bus = graph.nodes[f"{prefix}spm_connections"]
            self.assertEqual((cpu.name, arrays.name, scratch.name, router_spm.name),
                tuple(prefix + suffix for suffix in ("riscv", "arrays", "scratchpad", "router_spm")))
            self.assertEqual(router_spm.kind, "tilecomponents.MordredSpmEndpoint")
            self.assertEqual(router_spm.params, dict(tile_id=i, tile_count=4, spm_capacity_bytes=2097152,
                spm_request_bytes=32, spm_banks=4, spm_bank_width=4, router_spm_banks=[2, 3],
                request_window=4, max_request_bytes=256, memory_queue_depth=8, flit_size_bits=128, clock="1GHz",
                posted_receive_slots_per_source=16, posted_credit_batch=4, posted_credit_delay_cycles=4,
                net_command_queue_depth=4, net_ticket_capacity=16, net_transfers=[]))
            self.assertEqual(graph.links[f"{prefix}network_commands"].ends,
                ((cpu, "network_commands", "1ns"), (router_spm, "network_commands", "1ns")))
            self.assertEqual(graph.links[f"{prefix}spm_client_0"].ends,
                ((cpu.subcomponents["qemu_memory"], "lowlink", "1ns"), (bus, "highlink0", "1ns")))
            self.assertEqual(graph.links[f"{prefix}spm_client_1"].ends,
                ((router_spm.subcomponents["memory"], "lowlink", "1ns"), (bus, "highlink1", "1ns")))
            self.assertEqual(graph.links[f"{prefix}spm_controller"].ends,
                ((bus, "lowlink0", "1ns"), (scratch, "highlink", "1ns")))
            self.assertEqual(graph.links[f"{prefix}riscv_spm_commit"].ends,
                ((cpu, "external_commit", "0ps"), (scratch, "external_commit", "0ps")))
            self.assertEqual(scratch.params["external_write_requestor"], f"{prefix}riscv:qemu_memory")
            self.assertEqual(graph.links[f"{prefix}riscv_array_commands"].ends,
                ((cpu, "analog_commands", "1ns"), (arrays, "commands", "1ns")))
            self.assertIs(router_spm.subcomponents["networkIF"], result["mesh"]["nics"][i])
            self.assertIs(router_spm, result["mesh"]["endpoints"][i])
            path = tile["memory_file"]
            self.assertEqual(path, self.directory / f"tile{i}-spm.bin")
            self.assertEqual(path.stat().st_size, 2097152)
            self.assertEqual(path.read_bytes(), bytes(2097152))
            self.assertEqual(cpu.params["memory_file"], str(path))
            self.assertEqual(scratch.params["memory_file"], str(path))
            paths.add(path)
        self.assertEqual(len(paths), 4)
        # Verify independence after construction, not just distinct spellings.
        for i, tile in enumerate(result["tiles"]):
            with tile["memory_file"].open("r+b") as stream:
                stream.write(bytes([i + 1]))
        self.assertEqual([t["memory_file"].read_bytes()[0] for t in result["tiles"]], [1, 2, 3, 4])

    def test_arrays_have_only_cpu_command_links(self):
        graph = Composition()
        result = self.build(graph)
        for tile in result["tiles"]:
            links = [link for link in graph.links.values() if any(end[0] is tile["arrays"] for end in link.ends)]
            self.assertEqual(len(links), 1)
            self.assertEqual({end[0] for end in links[0].ends}, {tile["cpu"], tile["arrays"]})
            self.assertFalse(tile["arrays"].subcomponents)

    def test_general_mesh_uses_row_major_tile_ids(self):
        result = self.build(elfs=[self.elfs[0]]*6, mesh_parameters=dict(x_dim=3, y_dim=2), name="fabric")
        self.assertEqual(len(result["tiles"]), 6)
        for i, tile in enumerate(result["tiles"]):
            self.assertEqual(tile["router_spm"].params["tile_id"], i)
            self.assertEqual(tile["router_spm"].params["tile_count"], 6)
            self.assertEqual(tile["cpu"].name, f"fabric.tile{i}.riscv")
            self.assertEqual(result["mesh"]["routers"][i].name, f"fabric.router.{i % 3}.{i // 3}")

    def test_uniform_controls_are_forwarded_and_equivalent_clock_is_accepted(self):
        result = self.build(parameters=dict(spm_banks=2, spm_request_bytes=4, riscv_vector_length_bits=512),
            cpu_parameters=dict(load_store_queue_depth=4, analog_command_queue_depth=2),
            mesh_parameters=MeshParameters(clock="1000MHz", num_vcs=2),
            router_parameters=dict(max_request_bytes=33, memory_queue_depth=2, request_window=7))
        for tile in result["tiles"]:
            self.assertEqual(tile["cpu"].params["load_store_queue_depth"], 4)
            self.assertEqual(tile["arrays"].params["array_link_width"], 64)
            self.assertEqual(tile["router_spm"].params["spm_request_bytes"], 4)
            self.assertEqual(tile["router_spm"].params["max_request_bytes"], 33)
            self.assertEqual(tile["scratchpad"].subcomponents["backendConvertor"].subcomponents["backend"].params["spm_banks"], 2)

    def test_packet_capacity_accounts_header_rounding_and_minimum_two_flits(self):
        for payload, flit, capacity in ((1, 128, 112), (33, 128, 144), (1, 512, 128), (256, 128, 352)):
            with self.subTest(payload=payload, flit=flit):
                for transfers in (None, [], ()):
                    self.build(mesh_parameters=dict(flit_size_bits=flit, nic_output_buffer_bytes=capacity),
                               router_parameters=dict(max_request_bytes=payload), network_transfers=transfers)
                self.assert_rejected_without_mutation(
                    mesh_parameters=dict(flit_size_bits=flit, nic_output_buffer_bytes=capacity-flit//8),
                    router_parameters=dict(max_request_bytes=payload))

    def test_active_transfers_use_the_same_message_header(self):
        transfer=dict(transfer_id=7,source_tile=0,destination_tile=3,
                      receive_base=0x90080000,slot_capacity=4096,slot_count=1)
        arguments=dict(parameters=dict(router_spm_banks=[0,1,2,3]),network_transfers=[transfer])
        for payload, flit, capacity in ((1, 128, 112), (33, 128, 144), (1, 512, 128), (256, 128, 352)):
            with self.subTest(payload=payload, flit=flit):
                self.build(**arguments,
                    mesh_parameters=dict(flit_size_bits=flit, nic_output_buffer_bytes=capacity),
                    router_parameters=dict(max_request_bytes=payload))
                self.assert_rejected_without_mutation(**arguments,
                    mesh_parameters=dict(flit_size_bits=flit, nic_output_buffer_bytes=capacity-flit//8),
                    router_parameters=dict(max_request_bytes=payload))
        self.assert_rejected_without_mutation(**arguments,
            mesh_parameters=dict(flit_size_bits=128,nic_output_buffer_bytes=304),
            router_parameters=dict(max_request_bytes=256))

    def test_physical_bank_defaults_and_overrides_are_not_service_quotas(self):
        for count, expected in ((1, [0]), (2, [0, 1]), (4, [2, 3]), (8, [6, 7])):
            with self.subTest(count=count):
                result = self.build(parameters=dict(spm_banks=count))
                self.assertEqual(result["parameters"]["cpu_spm_banks"], list(range(count)))
                self.assertEqual(result["parameters"]["router_spm_banks"], expected)
        result = self.build(parameters=dict(spm_banks=4, cpu_spm_banks=[0, 2, 3], router_spm_banks=[0, 2]))
        for tile in result["tiles"]:
            scratch = tile["scratchpad"]
            backend = scratch.subcomponents["backendConvertor"].subcomponents["backend"]
            expected = dict(cpu_spm_banks=[0, 2, 3], router_spm_banks=[0, 2],
                cpu_requestor=tile["cpu"].name + ":qemu_memory",
                router_requestor=tile["router_spm"].name + ":memory")
            self.assertEqual({key: scratch.params[key] for key in expected}, expected)
            self.assertEqual({key: backend.params[key] for key in expected}, expected)
            self.assertEqual(tile["router_spm"].params["router_spm_banks"], [0, 2])
            self.assertNotIn("cpu_spm_banks", tile["arrays"].params)
            self.assertNotIn("router_spm_banks", tile["arrays"].params)
            self.assertEqual(backend.params["spm_channels"], 2)
            self.assertEqual(backend.params["spm_read_ports_per_bank"], 1)
            self.assertEqual(backend.params["spm_write_ports_per_bank"], 1)

    def test_mesh_snapshot_roundtrip_retains_explicit_bank_maps(self):
        resolved = self.build()["parameters"]
        self.assertEqual(self.build(parameters=resolved)["parameters"], resolved)
        for explicit in (dict(router_spm_banks=[]), dict(cpu_spm_banks=[]), resolve()):
            self.assert_rejected_without_mutation(parameters=explicit)

    def test_router_uses_only_guest_commands_without_a_control_region(self):
        graph = Composition()
        result = self.build(graph, parameters=dict(spm_capacity_bytes=4096))
        for tile in result["tiles"]:
            connected = {port for name, port in graph.connected if name == tile["router_spm"].name}
            self.assertEqual(connected, {"network_commands"})
            self.assertNotIn((tile["router_spm"].name, "requests"), graph.connected)
            self.assertNotIn((tile["router_spm"].name, "arrivals"), graph.connected)
            self.assertFalse({"control_offset", "poll_interval_cycles", "packet_payload_bytes"}
                             & tile["router_spm"].params.keys())

    def test_invalid_mesh_and_tile_controls_preserve_all_inputs(self):
        invalid = [dict(mesh_parameters=dict(local_ports=2)), dict(mesh_parameters=dict(num_vns=2)),
            dict(mesh_parameters=dict(clock="500MHz")), dict(mesh_parameters=dict(unknown=1)),
            dict(mesh_parameters=[]),
            dict(parameters=dict(spm_capacity_bytes=64*1024*1024)),
            dict(cpu_parameters=dict(load_store_queue_depth=0)),
            dict(cpu_parameters=dict(riscv_vector_length_bits=512), parameters=dict(riscv_vector_length_bits=256))]
        invalid += [dict(name=name) for name in ("", "bad name", None, 1)]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                self.assert_rejected_without_mutation(**arguments)

    def test_invalid_router_fields_preserve_all_inputs(self):
        invalid = []
        for field in ("max_request_bytes", "memory_queue_depth", "request_window",
                      "posted_credit_batch", "posted_credit_delay_cycles"):
            invalid += [{field: value} for value in (0, -1, True, 1.5, "8")]
        invalid += [dict(max_request_bytes=4097), dict(max_request_bytes=4096),
            dict(memory_queue_depth=1 << 32), dict(request_window=1 << 32),
            dict(tile_id=0), dict(tile_count=3), dict(flit_size_bits=64),
            dict(spm_capacity_bytes=4096), dict(spm_request_bytes=64), dict(clock="500MHz"),
            dict(router_spm_banks=[0, 1]), dict(spm_banks=8), dict(spm_bank_width=8),
            dict(control_offset=0x100000), dict(poll_interval_cycles=64), dict(packet_payload_bytes=256),
            dict(unknown=1), []]
        invalid += [dict(posted_receive_slots_per_source=value)
                    for value in (-1, True, 1.5, "8", 1 << 32)]
        for parameters in invalid:
            with self.subTest(parameters=parameters):
                self.assert_rejected_without_mutation(router_parameters=parameters)

    def test_posted_receive_capacity_can_be_disabled_or_configured(self):
        for slots in (0, 1, 16):
            result = self.build(router_parameters=dict(posted_receive_slots_per_source=slots,
                                posted_credit_batch=3, posted_credit_delay_cycles=7))
            for tile in result["tiles"]:
                self.assertEqual(tile["router_spm"].params["posted_receive_slots_per_source"], slots)
                self.assertEqual(tile["router_spm"].params["posted_credit_batch"], 3)
                self.assertEqual(tile["router_spm"].params["posted_credit_delay_cycles"], 7)

    def test_invalid_bank_ids_preserve_all_inputs(self):
        for field in ("cpu_spm_banks", "router_spm_banks"):
            for value in ([4], [-1], [0, 0], [True], [0.0], "0,1", {0, 1}, ["0"]):
                with self.subTest(field=field, value=value):
                    self.assert_rejected_without_mutation(parameters={field: value})

    def test_deployment_reserves_only_active_transfers_and_separates_producers(self):
        a=dict(transfer_id=(1<<63)-1,source_tile=0,destination_tile=3,
               receive_base=0x90080000,slot_capacity=4096,slot_count=2)
        b=a|dict(transfer_id=0,source_tile=1,receive_base=0x90082000,slot_count=1)
        result=self.build(parameters=dict(router_spm_banks=[0,1,2,3]),network_transfers=[a,b])
        expected=[list(a.values()),list(b.values()),[],list(a.values())+list(b.values())]
        self.assertEqual([tile['router_spm'].params['net_transfers'] for tile in result['tiles']],expected)
        for tile in result['tiles']:
            self.assertNotIn('net_receive_bases',tile['router_spm'].params)

    def test_invalid_deployments_preserve_backing_and_topology(self):
        a=dict(transfer_id=7,source_tile=0,destination_tile=3,
               receive_base=0x90080000,slot_capacity=4096,slot_count=2)
        invalid=[{}, {}, a|dict(unknown=1)]
        for field,values in dict(transfer_id=[-1,1<<63,True,'7'], source_tile=[-1,4,3,True],
                destination_tile=[-1,4,0],receive_base=[0,0x90000001,0x90200000,0x901ffff8],
                slot_capacity=[0,-1,1<<63,1<<20],slot_count=[0,257,-1,True]).items():
            invalid.extend(a|{field:value} for value in values)
        for record in invalid:
            with self.subTest(record=record):
                self.assert_rejected_without_mutation(parameters=dict(router_spm_banks=[0,1,2,3]),
                    network_transfers=[record])
        for records in ([a,a], [a,a|dict(source_tile=1,receive_base=0x900a0000)],
                        [a,a|dict(transfer_id=8,source_tile=1)], list(a), {'a':a}):
            self.assert_rejected_without_mutation(parameters=dict(router_spm_banks=[0,1,2,3]),network_transfers=records)
        self.assert_rejected_without_mutation(network_transfers=[a])  # NIU bank permissions
        self.assert_rejected_without_mutation(parameters=dict(router_spm_banks=[0,1,2,3],cpu_spm_banks=[0,1]),
            network_transfers=[a])
        self.assert_rejected_without_mutation(parameters=dict(router_spm_banks=[0,1,2,3]),network_transfers=[a],
            router_parameters=dict(posted_receive_slots_per_source=0))

    def test_removed_directional_controls_and_invalid_capacities_are_rejected(self):
        for options in (dict(net_connections=4),dict(mesh_x_dim=2),dict(net_receive_slots=2),
                        dict(net_receive_bases=[0,0,0,0]),dict(net_receive_slot_bytes=4096),
                        dict(net_ticket_capacity=1),dict(net_ticket_capacity=1025),dict(net_command_queue_depth=0)):
            self.assert_rejected_without_mutation(router_parameters=options)

    def test_later_missing_elf_is_checked_before_first_backing_is_truncated(self):
        self.assert_rejected_without_mutation(elfs=[*self.elfs[:3], self.root / "missing.elf"])
        self.assert_rejected_without_mutation(qemu=self.root / "missing-qemu")

    def test_explicit_elf_sequence_is_required(self):
        for elfs in ([], self.elfs[:3], [*self.elfs, self.elfs[0]], str(self.elfs[0]), iter(self.elfs)):
            with self.subTest(elfs=elfs):
                self.assert_rejected_without_mutation(elfs=elfs)

    def test_backing_paths_cannot_alias_any_elf_or_qemu(self):
        self.directory.mkdir()
        later = self.directory / "tile3-spm.bin"
        for source in (self.elfs[0], self.elfs[3], self.qemu):
            for kind in ("symlink", "hardlink"):
                with self.subTest(source=source, kind=kind):
                    if kind == "symlink":
                        later.symlink_to(source)
                    else:
                        os.link(source, later)
                    self.assert_rejected_without_mutation()
                    later.unlink()

    def test_backing_paths_cannot_alias_one_another(self):
        self.directory.mkdir()
        first = self.directory / "tile0-spm.bin"
        first.write_bytes(b"preserve")
        later = self.directory / "tile3-spm.bin"
        for kind in ("symlink", "hardlink"):
            with self.subTest(kind=kind):
                if kind == "symlink":
                    later.symlink_to(first)
                else:
                    os.link(first, later)
                self.assert_rejected_without_mutation()
                later.unlink()

    def test_invalid_output_paths_preserve_existing_backing(self):
        self.directory.mkdir()
        (self.directory / "tile3-spm.bin").mkdir()
        self.assert_rejected_without_mutation()
        self.assert_rejected_without_mutation(memory_directory=self.qemu / "child")

    def test_later_symlink_target_with_file_parent_is_rejected_before_truncation(self):
        self.directory.mkdir()
        (self.directory / "tile3-spm.bin").symlink_to(self.qemu / "not-a-directory")
        self.assert_rejected_without_mutation()

    def test_two_named_tile_meshes_can_share_one_sst_graph(self):
        graph = Composition()
        a = self.build(graph, name="a", memory_directory=self.root / "a")
        b = self.build(graph, name="b", memory_directory=self.root / "b")
        self.assertEqual((len(graph.nodes), len(graph.links)), (48, 64))
        self.assertFalse({t["memory_file"] for t in a["tiles"]} & {t["memory_file"] for t in b["tiles"]})

    def test_default_tile_names_and_links_remain_unchanged(self):
        graph = Composition()
        cpu, arrays, scratch = connect_riscv_arrays(graph, {}, elf=self.elfs[0], qemu=self.qemu,
                                                   memory_file=self.root / "single.bin")
        self.assertEqual(set(graph.nodes), {"riscv", "arrays", "scratchpad", "spm_connections"})
        self.assertEqual(set(graph.links), {"spm_client_0", "spm_controller", "riscv_spm_commit", "riscv_array_commands"})
        self.assertEqual((cpu.name, arrays.name, scratch.name), ("riscv", "arrays", "scratchpad"))
        self.assertEqual(scratch.params["external_write_requestor"], "riscv:qemu_memory")

    def test_prefix_is_consistent_through_every_public_composition_entry(self):
        entries = (connect, connect_arrays, connect_scratchpad, _connect_scratchpad, connect_riscv, connect_riscv_arrays)
        for entry in entries:
            with self.subTest(entry=entry.__name__):
                graph = Composition()
                for index in range(2):
                    arguments = dict(name_prefix=f"tile{index}.")
                    if entry in (connect_riscv, connect_riscv_arrays):
                        arguments.update(elf=self.elfs[index], qemu=self.qemu,
                                         memory_file=self.root / f"single{index}.bin")
                    if entry is _connect_scratchpad:
                        entry(graph, resolve(), (), **arguments)
                    else:
                        entry(graph, {}, **arguments)
                self.assertTrue(all(name.startswith(("tile0.", "tile1.")) for name in [*graph.nodes, *graph.links]))
                self.assertEqual(sum(name.startswith("tile0.") for name in graph.nodes),
                                 sum(name.startswith("tile1.") for name in graph.nodes))

    def test_invalid_prefix_is_rejected_before_file_or_topology_changes(self):
        for prefix in (None, 1, "bad prefix", "tile\n"):
            graph = Composition()
            path = self.root / "single.bin"
            path.write_bytes(b"preserve")
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                connect_riscv_arrays(graph, {}, elf=self.elfs[0], qemu=self.qemu,
                                     memory_file=path, name_prefix=prefix)
            self.assertEqual(path.read_bytes(), b"preserve")
            self.assertFalse(graph.nodes)


if __name__ == "__main__":
    unittest.main()
