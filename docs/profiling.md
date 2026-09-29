# Optional cycle profiling

Profiling is **off by default**. Enable it when inspecting a run, then disable
it for normal simulation. The controls select observations, not hardware
parameters or a different scheduling policy.

After the normal hardware build, run an SST configuration with:

```sh
bash tools/hardware/env.sh --profile /tmp/tile-profile sst your-simulation.py
bash tools/hardware/env.sh --no-profile sst your-simulation.py
```

The equivalent environment variables are `TILE_CYCLE_PROFILE=1` and
`TILE_CYCLE_PROFILE_DIRECTORY=/tmp/tile-profile`. Set `TILE_CYCLE_PROFILE=0`
or leave it unset to disable profiling. A directory setting alone never enables
it. Values other than `0` and `1` are rejected. Use a separate directory per run;
opening a component trace replaces that file.

A Python configuration can call this before creating any components, with `src`
on its import path:

```python
from profiling import configure

configure(enabled=True, output_directory="/tmp/tile-profile")
# configure(enabled=False) disables it again.
```

The maintained runner also accepts the flag:

```sh
bash tests/run-all.sh --case network/mordred-spm --profile
bash tests/run-all.sh --case platform/profiling
```

The runner puts component state traces in each simulation's `profiles/`
directory. It keeps the existing CPU and event CSVs beside the other test
evidence. Those existing correctness diagnostics are still available with
profiling disabled. A standalone configuration can collect both kinds of trace
using the explicit output directory above, without setting test variables.

## Recorded observations

| Component | Observations |
| --- | --- |
| RISC-V CPU | Instruction segments, instruction-cache accesses, memory operations, LSQ and analog-command-queue events, and recorded waits |
| Analog accelerator | Command occupancy, active computation, result slots and command events |
| Shared SPM | Controller ordering/admission, connection request queues, bank service and channel activity |
| NIU | Memory fragments, pending messages and receive/storage credits |
| Mordred NIC and router ports | Packet/flit queues, credits, flit sends and receives |
| Router | Arbitration and per-port activity |

State/event profiles are named `<component>-cycles.csv` and use
`cycle,kind,resource,index,value,token,detail`. State records describe occupancy
at observation points; event records describe transitions. Their timestamp is
simulation time in nanoseconds, equal to tile cycles at the supported 1 GHz
tile clock. Network components also use that time base even when their clock
is configured differently. Existing CPU/event CSVs retain their documented
schemas. These are observations of the modeled components, not an additional
physical component or a model of every internal signal.

No profiling clock, event or simulated latency is added. The observed SPM
connection component is generated from the pinned SST Bus source with passive
recording hooks. Mordred hooks are applied to a build-local copy of its pinned
source. Both are included in the ordinary simulator build. Recording can
increase host runtime and disk use; disabled runs do not create profile files.

The [profiling regression](../src/tests/profiling/README.md) runs the maintained
four-tile fixture with profiling unset, explicitly off, and on. It compares
results, timing and event traces, checks network flit conservation and physical
link timing, and repeats the enabled run with a one-instruction QEMU grant.
The trace files are the supported output; this release adds no dashboard viewer.
