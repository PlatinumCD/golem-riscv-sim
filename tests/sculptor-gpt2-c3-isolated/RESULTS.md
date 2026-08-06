# Isolated GPT-2 C3 Result

## Scope

This test runs task 130 from decoder block 0 on one tile.
The test compares the compiler baseline with the C3 elementwise-slice rewrite.
It does not run the complete GPT-2 deployment.

Both versions use the same deterministic input bytes.
Both versions load the same analog arrays during boot.

## Correctness

Both versions produced checksum `0x19f79925`.
Both versions completed without an execution error.

The compiler rewrite did not change these deployment properties:

- The route manifest contains the same 1,330 routes.
- The deployment contains the same 62 active cores.
- The partitioned graph contains the same 2,660 task operations.

## Native memory

The native backend does not add memory-service time.

| Version | Simulated time | Instructions |
|---|---:|---:|
| Baseline | 2.239143 ms | 4,476,236 |
| C3 | 2.244076 ms | 4,486,102 |

The C3 version was 0.004933 ms slower in this control test.
The increase was 0.2203 percent.

The C3 version used 9,866 more instructions.
Four smaller elementwise loops caused this instruction increase.

## Private L1 and shared L2

| Version | Simulated time | L1 hits | L1 misses | L2 hits | L2 misses |
|---|---:|---:|---:|---:|---:|
| Baseline | 5.786966 ms | 373,712 | 10,371 | 5,790 | 4,581 |
| C3 | 5.633945 ms | 362,784 | 9,006 | 5,190 | 3,816 |

The C3 version was 0.153021 ms faster with the memory hierarchy.
The decrease was 2.6442 percent.

The C3 version removed these memory events:

- 10,928 L1 hits.
- 1,365 L1 misses.
- 600 L2 hits.
- 765 L2 misses and lower-memory requests.

## Task 130 `memcpy` result

| Metric | Baseline | C3 | Difference |
|---|---:|---:|---:|
| Binary caller sites | 44 | 40 | -4 |
| Invocations | 44 | 40 | -4 |
| Request events | 40,960 | 28,672 | -12,288 |
| Read-and-write bytes | 327,680 | 229,376 | -98,304 |
| Stall ticks | 460,232,000 | 294,582,000 | -165,650,000 |

The four removed copies each moved 12,288 source bytes.
Each copy generated 24,576 bytes of read-and-write traffic.

## Result

The rewrite exchanges instruction overhead for less memory traffic.
The native backend sees only the instruction overhead.
The memory hierarchy sees a net 2.6442 percent time decrease.

This result proves that the L1 and L2 model changes the compiler decision.
An instruction-only score rejects this rewrite, but the timed memory model accepts it.

## Data

The generated files are in `build/tests/sculptor-gpt2-c3-isolated`.
The caller data is in each `v2-*-shared-l2/profile/memory-caller-sites.csv` file.
