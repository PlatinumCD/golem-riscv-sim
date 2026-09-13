# CPU execution and clocks

| File | Responsibility |
|---|---|
| [cpuExecutionController.cc](cpuExecutionController.cc) | Consume stops, charge instruction work, and dispatch timed actions |
| [cpuExecutionLedger.h](cpuExecutionLedger.h) | Incremental scalar/vector issue accounting across grants |
| [qemuProcess.cc](qemuProcess.cc) | Launch and stop the managed QEMU process |
| [qemuCaptureCoordinator.cc](qemuCaptureCoordinator.cc) | Coordinate capture of QEMU stop records |
| [clockDomain.h](clockDomain.h) | Checked conversions between resource cycles and SST time |

The CPU executes from SPM with single-issue, instruction-ordered replay. Resource timing
belongs to memory, network and analog controllers; this directory coordinates
when the CPU may continue.

## Clock domains

`Timing::Ticks` is absolute SST core time; its unit is the configured SST
timebase, not an assumed nanosecond. `Clock<Domain>` obtains its tick period
from SST's actual `TimeConverter`.

- CPU instruction accounting and CPU-facing SPM service use CPU cycles.
- Shared-memory DMA service uses its controller clock.
- SPM-targeted RX/TX use the common SPM timing model in CPU cycles.
- Analog progress is the number of complete analog cycles elapsed (floor).
  Analog wakeups use checked analog-cycle-to-tick conversion.
- NoC completion work converts link cycles to ticks; accumulated packet transit
  time is not an extra wall-clock interval to add to elapsed execution time.

Typed cycle values cannot implicitly cross domains. Boundary conversion goes
through `Ticks`. Multiplication and addition reject overflow; ceil conversion
does not use the overflowing `(value + divisor - 1)` idiom. Guest ABI counters
stay integer-valued; types constrain the simulator-side conversion points.

Service cycles and stall sums may overlap. Neither should be summed into elapsed
runtime without a disjoint-interval proof. The progress watchdog is host wall
time; its polling interval is one million CPU cycles, 1 ms only at 1 GHz.

See the [timing model](../../../docs/timing-model.md) for formulas and
[measurement rules](../profiling/MEASUREMENT_CONTRACT.md) for counter meanings.
