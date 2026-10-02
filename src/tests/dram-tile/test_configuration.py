"""Reject invalid DRAM compositions before creating components or touching inputs."""
import importlib.util
from pathlib import Path
import sys
import unittest

HERE=Path(__file__).resolve().parent
SOURCE=HERE.parents[1]
sys.path.insert(0,str(SOURCE))
from components.dram_tile.configuration import DramParameters
spec=importlib.util.spec_from_file_location('mesh_configuration_tests',SOURCE/'tests/network-instructions/test_configuration.py')
mesh=importlib.util.module_from_spec(spec); spec.loader.exec_module(mesh)


class DramConfigurationTests(mesh.TileMeshConfigurationTests):
    # The inherited mesh regressions also exercise the default compute-only path.
    def setUp(self):
        super().setUp()
        self.image=self.root/'weights.bin'
        with self.image.open('wb') as stream: stream.truncate(1024*1024)
        self.inputs[self.image]=self.image.read_bytes()
        self.dram=dict(image=self.image,capacity_bytes=1024*1024)

    def test_dram_tile_uses_control_cpu_and_separate_timed_memory(self):
        graph=mesh.Composition()
        result=self.build(graph,dram_tiles={0:self.dram})
        tile=result['tiles'][0]
        self.assertEqual(tile['router_spm'].kind,'tilecomponents.DramTile')
        self.assertIsNone(tile['arrays'])
        self.assertNotIn('tile_mesh.tile0.arrays',graph.nodes)
        self.assertNotIn((tile['cpu'].name,'analog_commands'),graph.connected)
        self.assertEqual(tile['dram_controller'].kind,'memHierarchy.MemController')
        self.assertEqual(tile['dram_backend'].kind,'memHierarchy.timingDRAM')
        self.assertEqual(tile['dram_backend'].params['mem_size'],'1048576B')
        self.assertEqual(tile['dram_backend'].params['channels'],2)
        self.assertEqual(tile['dram_backend'].params['max_requests_per_cycle'],2)
        self.assertIn('dram_memory',tile['router_spm'].subcomponents)
        self.assertTrue(all(tile['arrays'] is not None for tile in result['tiles'][1:]))
        for item in result['tiles']:
            backend=item['scratchpad'].subcomponents['backendConvertor'].subcomponents['backend']
            self.assertEqual(backend.params['spm_write_ports_per_bank'],1)
        self.assertEqual(self.image.read_bytes(),self.inputs[self.image])

    def test_dram_invalid_tile_ids_and_parameters_are_atomic(self):
        for value in ({True:self.dram},{-1:self.dram},{4:self.dram},[],{0:dict(self.dram,unknown=3)},
                      {0:dict(self.dram,base=0x90000000)},{0:dict(self.dram,capacity_bytes=128)},
                      {0:dict(self.dram,image=self.root/'missing')},{0:dict(self.dram,request_bytes=128)}):
            with self.subTest(value=value): self.assert_rejected_without_mutation(dram_tiles=value)

    def test_dram_image_must_not_alias_spm_or_dram_outputs(self):
        self.directory.mkdir()
        for suffix in ('spm','dram'):
            alias=self.directory/f'tile0-{suffix}.bin'
            alias.hardlink_to(self.image)
            with self.subTest(suffix=suffix):
                graph=mesh.Composition()
                with self.assertRaises(ValueError): self.build(graph,dram_tiles={0:self.dram})
                self.assertFalse(graph.nodes)
                self.assertEqual(self.image.read_bytes(),self.inputs[self.image])
            alias.unlink()

    def test_dram_output_must_not_alias_other_tile_output(self):
        self.directory.mkdir()
        a=self.directory/'tile0-dram.bin'; a.write_bytes(b'existing output')
        (self.directory/'tile1-spm.bin').hardlink_to(a)
        graph=mesh.Composition()
        with self.assertRaises(ValueError): self.build(graph,dram_tiles={0:self.dram})
        self.assertEqual(a.read_bytes(),b'existing output')
        self.assertFalse(graph.nodes)

    def test_dram_geometry_and_timing_validation(self):
        for values in (dict(channels=0),dict(channels=True),dict(queue_depth=0),dict(request_bytes=3),
                       dict(row_bytes=65),dict(row_bytes=192),dict(base=(1<<63)-64),
                       dict(channels=64,ranks_per_channel=16,banks_per_rank=256,row_bytes=1<<20,request_bytes=1),
                       dict(capacity_bytes=1<<60),dict(t_cas_cycles=0),dict(clock='0GHz')):
            with self.subTest(values=values),self.assertRaises(ValueError): DramParameters(**values)
        self.assertEqual(DramParameters(channels=1,clock='800MHz').channels,1)

    def test_spm_output_symlink_does_not_change_dram_output_path(self):
        self.directory.mkdir()
        target=self.root/'control-memory.bin'
        target.write_bytes(b'prior output')
        (self.directory/'tile0-spm.bin').symlink_to(target)
        tile=self.build(dram_tiles={0:self.dram})['tiles'][0]
        self.assertEqual(tile['memory_file'],target)
        self.assertEqual(tile['dram_memory_file'],self.directory/'tile0-dram.bin')
        self.assertNotEqual(tile['dram_controller'].params['backing_out_file'],str(target))


if __name__=='__main__': unittest.main()
