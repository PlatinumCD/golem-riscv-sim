"""The measurement checker must reject missing, rounded and misattributed traffic."""
import unittest
from check import validate_dma_totals


class DMACheckTest(unittest.TestCase):
    def setUp(self):
        self.data=65548
        self.boot=512
        self.traffic={p:dict(memory_to_spm_bytes=r,spm_to_memory_bytes=w)
                      for p,r,w in ((0,512,0),(1,0,self.data),
                                    (2,self.data,self.data),(3,self.data,0))}
        self.summary=dict(physical_global_dma_submitted=68,physical_global_dma_completed=68,
                          scratchpad_dma_bytes=4*self.data+self.boot,network_packets=0)

    def validate(self):
        return validate_dma_totals(self.traffic,self.summary,self.data,4096,self.boot)

    def test_partial_chunk(self):
        self.assertEqual(self.validate(),17)

    def test_boot_is_not_workload(self):
        self.traffic[2]['memory_to_spm_bytes']+=self.boot
        with self.assertRaises(AssertionError): self.validate()

    def test_tail_cannot_be_rounded_up(self):
        self.traffic[2]['spm_to_memory_bytes']+=4096-12
        with self.assertRaises(AssertionError): self.validate()

    def test_missing_counter_is_not_zero(self):
        del self.summary['physical_global_dma_completed']
        with self.assertRaises(KeyError): self.validate()

    def test_incomplete_dma_fails(self):
        self.summary['physical_global_dma_completed']-=1
        with self.assertRaises(AssertionError): self.validate()


if __name__=='__main__': unittest.main()
