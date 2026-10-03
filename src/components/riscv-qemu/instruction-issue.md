# In-order instruction issue

Set `cpu_parameters={"issue_width": 2}` to enable dual issue. The default remains
one instruction per cycle. Widths 1 through 4 are accepted; every instruction,
including arithmetic, branches, RVV and custom instructions, consumes one slot.

QEMU supplies the actual translated instruction bits and the pre-instruction
VTYPE at every fetch rendezvous. SST decodes operand sets, checks register
readiness and execution-unit capacity, then permits that instruction to execute.
QEMU remains the functional ISA implementation. Bridge version 37 rejects an
older QEMU/SST pairing rather than silently omitting operand metadata.

The scheduler checks RAW and WAW dependencies across integer, FP and vector
registers, including LMUL/EMUL groups, masks and VL/VTYPE. Operands are captured
in order, so later writes cannot change earlier computations' inputs. Existing
scalar/RVV LSQs and analog-queue barriers remain authoritative for asynchronous
loads, memory ordering and register ownership. A modeled one-cycle memory issue
stage never makes a load's value available before its actual SPM completion.

| Resource | Ports | Result latency | Initiation interval |
|---|---:|---:|---:|
| Integer ALU | `integer_issue_units=2` | `integer_latency_cycles=1` | `integer_initiation_interval=1` |
| Scalar/RVV memory instruction | `memory_issue_units=1` | Real memory/queue completion | 1 |
| RVV arithmetic | 1 | `vector_latency_cycles=1` | `vector_initiation_interval=1` |
| Scalar FP arithmetic | 1 | `floating_latency_cycles=3` | `floating_initiation_interval=1` |
| Scalar integer multiply | 1 | `multiply_latency_cycles=3` | 1 |
| Scalar divide and FP divide/sqrt | 1 | `divide_latency_cycles=16` | Same as latency |
| Analog/network command | 1 | Existing device admission/completion contract | 1 |

These are configurable timing assumptions, not calibration to a particular
commercial CPU. Memory-instruction ports constrain scalar/RVV instruction
admission, not the existing SPM beat/bank service. RVV arithmetic latency is
currently per instruction; it does not model element lanes or vary with VL.
Some complex vector forms reserve conservative register supersets. System,
atomic and unclassified encodings serialize; QEMU still validates their legality.
There is no speculation, register renaming, reorder buffer, branch predictor or
separate retirement pipeline. A blocked oldest instruction holds up younger
instructions while memory, network and analog events continue progressing.

## Fetch and timing

`instruction_fetch_width=0` follows `issue_width`. An explicit value from 1 to 4
limits the number of sequential instructions supplied by one cached fetch block.
A block pays `instruction_cache_hit_cycles` once. Its additional instructions
can issue in the same availability cycle, subject to the scheduler. A branch,
custom control instruction, cache-line crossing or `fence.i` ends the block.
Unused buffered instructions remain available across dependency/unit stalls.
Each instruction still has its own QEMU handshake; execution is
never batched past an unchecked dependency or side effect. Uncached fetches keep
their actual SPM request/response timing. Cache misses and refill traffic are
unchanged. Increasing issue width without increasing fetch or unit bandwidth
therefore cannot manufacture throughput.

Host synchronization grants do not define issue boundaries. Budgets 1, 7 and
256 produce identical simulated schedules. The old count/width ledger is kept
only for validating retired instruction counts; it no longer advances time.

## Observations

With the existing profiling/output flag enabled, `<cpu>-issue.csv` records actual
instruction admission (`cycle,pc,instruction,unit,pipeline_ready_cycle`). The ready
column is the scheduled arithmetic result time, or the issue-stage completion
time for memory/device instructions. Their actual result/ownership completion
must be read from the existing LSQ/analog/network traces. It is not retirement.
`<cpu>-issue-waits.csv` records width, register, unit and serialization waits.
Overlapping constraints receive one controlling reason; their counts are not
independent additive resource-utilization measures.

`RISCV_STATS` adds `issue_width`, effective `instruction_fetch_width`,
`issued_instructions`, `peak_issue_width` and `instruction_issue_wait_cycles`.
`issue_cycles` now counts cycles with an actual admission. Instruction-cache
events remain fetch observations and may precede issue when an operand/unit is
unavailable. Consumers needing actual issue timing should use the new issue CSV.

The [regression suite](../../tests/instruction-issue/README.md) covers throughput,
dependencies, configured latency/intervals, fetch limits and host-budget invariance.
