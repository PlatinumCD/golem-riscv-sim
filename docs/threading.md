# Running one mesh simulation across SST threads

Use the usual configuration that calls `connect_riscv_mesh`, and select the
number of SST workers at launch:

```sh
sst --num-threads=4 simulation.py
```

The mesh helper selects `sst.self` and assigns each complete tile to one
worker. Its CPU, arrays, SPM, SPM connections, NIU, DRAM controller when present,
and router stay together. Subcomponents inherit their parent's placement.
Only inter-router links cross workers; their modeled latency is unchanged.
QEMU still runs in one separate process per tile CPU.

The default is one SST thread. More threads accelerate host execution without
changing simulated hardware parameters. Use one MPI rank, at least one thread,
and no more threads than physical tiles. A 2×2 mesh supports up to four workers;
a 4×4 mesh supports up to sixteen. The automatic placement recursively divides
the mesh into spatial rectangles. It does not estimate application load.

For a custom assignment, pass `tile_threads` in row-major physical tile order:

```python
network = connect_riscv_mesh(
    sst, parameters, elfs=tile_elfs, memory_directory=run_directory,
    mesh_parameters=dict(x_dim=2, y_dim=2),
    tile_threads=[0, 0, 1, 1],  # launch this configuration with two SST threads
)
```

The resolved assignment is returned as `network["tile_threads"]`. Worker IDs
must be valid for the launched thread count. Keep the helper's `sst.self`
partitioner and component placements; later overrides can break shared-SPM
ordering. Independently built CPU tiles remain guarded against unconfigured
multi-thread execution. No experimental environment variable is required.

Cycle profiling remains optional. Full-run component traces and cycle
profiles are supported. `TILE_COMPONENT_TRACE_START_TASK` is rejected with
multiple threads because its process-global gate cannot represent independent
per-tile task boundaries. MPI execution is also rejected.

The maintained [threading regressions](../src/tests/sst-threads/README.md)
compare results, complete component statistics, SPM images, queue drainage,
receive ownership and actual SST placement against one-thread executions.
They cover compute-only and mixed DRAM/compute meshes and profiling on/off.
Host speedup depends on workload dependencies, placement and host resources;
more workers do not guarantee a faster run.
