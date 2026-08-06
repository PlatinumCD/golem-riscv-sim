# GPT-2 Greedy-L3 Tail-Completion Validation

**Date:** 2026-07-30  
**Model:** GPT-2-small fixture, four tokens  
**Placement:** Greedy, transfer-cost, lookahead 3, beam 1, diagonal scope  
**Hardware:** 8x8 mesh, 61 active tiles, 32-bit 1 GHz links, dual-issue
1 GHz CPUs, native memory  
**Result:** Analog and digital both passed

## Purpose

This focused regression tests whether the corrected tail-complete network
boundary remains functional and changes timing sensibly in a complete
compiler/runtime/QEMU/SST deployment.

The existing Greedy-L3 compiler objects were reused. Fresh ELFs and simulation
logs were written outside the 112-result scheduling sweep.

## Ordering issue found by the digital run

The first analog run passed, but the first digital run exposed an invalid
receive-DMA association:

```text
tile 1 RX DMA burst from tile 0 has 512 words,
but route 19 expects only 507 more
```

The initial tail fix delayed every packet independently. Under clustered
Merlin head callbacks, this allowed a later short packet to complete before an
earlier long packet:

```text
incorrect
---------
long packet head  ---- long tail delay ----------> complete
short packet head   -- short tail delay --> complete
                                            ^
                                            impossible overtaking

correct
-------
long packet  |<--------- serialized --------->|
short packet                                      |<-- serialized -->|
```

The receiver now owns one physical-link next-available timestamp:

```text
start = max(head arrival, next available)
complete = start + packet cycles - 1
next available = start + packet cycles
```

This retains head latency and Merlin arbitration while ensuring that the
32-bit destination link cannot complete two packets out of physical order.

## Results

| Backend | Previous time | Corrected time | Increase | Relative increase | Status |
|---|---:|---:|---:|---:|---|
| Analog | 19.2038 ms | 19.297 ms | 93.2 us | 0.485% | Pass |
| Digital | 118.106 ms | 118.228 ms | 122.0 us | 0.103% | Pass |

The corrected analog speedup over digital is:

```text
118.228 / 19.297 = 6.127x
```

The prior estimate was 6.150x. Tail-complete visibility therefore changes the
reported speedup by only 0.38% in this case, while removing an unphysical
early-arrival path.

## Functional and traffic checks

| Backend | Elements | Finite | First bits | Checksum bits | Transmitted words |
|---|---:|---:|---:|---:|---:|
| Analog | 3,072 | 3,072 | 3,218,958,785 | 1,003,055,360 | 801,478 |
| Digital | 3,072 | 3,072 | 3,218,958,914 | 3,139,810,816 | 801,478 |

Each backend's output signature exactly matches its corresponding result from
before the timing correction. Analog and digital differ slightly because they
use different numerical implementations; this comparison checks stability
within each backend.

The corrected aggregate activity is:

| Backend | Retired instructions | Vector instructions | CPU cycles | Analog-active cycles |
|---|---:|---:|---:|---:|
| Analog | 53,334,107 | 8,650,929 | 26,669,380 | 15,920,600 |
| Digital | 328,638,200 | 128,606,241 | 164,320,436 | 0 |

## Artifacts

```text
build/tests/network-tail-gpt2-greedy-l3-20260730/
  analog/simulation.log
  digital/simulation.log
```

## Conclusion

The packet-completion correction survives a complete 61-active-core GPT-2
analog/digital comparison. It modestly increases simulated time, preserves
all functional signatures and communication volume, and prevents impossible
variable-length packet overtaking before receive DMA.
