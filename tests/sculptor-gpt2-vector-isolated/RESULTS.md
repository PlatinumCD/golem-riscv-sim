# GPT-2 Task 152 Vector Result

## Scope

This test runs one `digital.residual_add` task from GPT-2 decoder block 0.
The task adds two `tensor<1x4x768xf32>` inputs.
The task sends the same result to two output resources.

The test uses one tile with a private L1 cache and a shared L2 cache.
The memory interface uses the current blocking request model.
The test does not run the complete GPT-2 deployment.

## Correctness

The scalar and vector ELFs produced checksum `0xd8a0ef45`.
The test also compared each output value with the expected addition result.
All four resource bases had 32-byte alignment.

The corrected vector task used the same storage plan as the scalar task.
Each task wrote directly to output slot 166.
Each task used one `memcpy` call to copy the result to output slot 167.
The vector task did not use `malloc` or `free`.

## Task result

| Metric | Scalar | Vector | Difference |
|---|---:|---:|---:|
| Task time | 155.362 us | 145.763 us | -9.599 us |
| Retired instructions | 25,490 | 3,987 | -21,503 |
| CPU cycles | 12,858 | 3,259 | -9,599 |
| Timed memory events | 12,360 | 12,360 | 0 |
| Timed memory bytes | 61,949 | 61,949 | 0 |
| Memory stall time | 142.504 us | 142.504 us | 0 |

The vector task decreased the task time by 6.18 percent.
The measured task speedup was 1.066 times.
The vector task decreased the instruction count by 84.36 percent.

## Vector contract

The vector task executed 768 `vle32.v` instructions.
It executed 384 `vse32.v` instructions.
Thus, it executed 1,152 vector memory instructions.

QEMU expanded each vector memory instruction into eight ordered 4-byte events.
The 1,152 instructions produced 9,216 timed events and 36,864 bytes.

The task produced 12,288 resource events and 61,440 resource bytes.
The additional resource traffic came from the required output fanout copy.
Stack and descriptor accesses produced the remaining 72 events and 509 bytes.

## Memory result

The two versions had the same memory event count and latency distribution.
Each version had 11,570 events with 5 ns latency.
Each version had 790 events with longer latency.

The vector task did not decrease memory stall time in this experiment.
It decreased only the instruction and CPU-cycle costs.
After vectorization, memory stalls were 97.76 percent of the task time.

## Conclusion

This result separates compiler vectorization from memory overlap.
The compiler change removes 84.36 percent of the task instructions.
The current blocking memory model limits the task speedup to 6.18 percent.

The next experiment must use the same two ELFs with a pipelined L1 model.
That experiment will measure the additional benefit from memory overlap.

The machine-readable result is in `build/tests/sculptor-gpt2-vector-isolated/result.json`.
