# Clock domains

`Timing::Ticks` is absolute SST core time; its unit is the configured SST
timebase, not an assumed nanosecond. `Clock<Domain>` obtains its tick period
from SST's actual `TimeConverter`.

- CPU instruction accounting and CPU-facing SPM service use CPU cycles.
- Ordinary-memory RX DMA schedules on `rx_dma_clock`; transfer records and
  CPU wakeups convert to CPU cycles, rounding start and completion upward.
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
