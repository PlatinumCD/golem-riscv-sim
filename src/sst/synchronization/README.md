# Startup and epoch barriers

The default epoch-zero barrier waits for every active tile. Later epochs mark
initialization and execution boundaries supplied by the guest program.

For preloaded shared-RAM workloads, `local_boot_release=true` on the epoch
controller acknowledges epoch zero separately for each tile. Early tiles can
initialize while other tiles boot. Epoch one and subsequent epochs remain
global. Guest epoch numbers and ELFs do not change. Local acknowledgements are
logged separately; the controller's `releases` statistic counts global releases.
`stop_after_releases` retains its existing logical-epoch cutoff (2 means stop
after epoch one), even when epoch zero is acknowledged locally.

The mesh builder requires a preloaded RAM image for this option and enables
`epoch_barrier_drain_analog` on tiles. That flag delays barrier arrival until all
local submitted analog commands complete; completions wake the waiting CPU,
without polling or invented fixed delays. Explicit global-DMA token retirement
is still required. The flag may also be enabled with the default global boot
barrier, for a controlled scheduling comparison.

This does not bypass communication dependencies or make uninitialized data
ready. Only opt in when initialization inputs are independently available.
It does not change analog instruction submission semantics or introduce a
general-purpose guest analog fence for arbitrary staging-buffer reuse.

Tests: `tests/local_boot_barrier.py` exercises staggered boot arrivals;
`tests/device_owners.py` checks completion-gated arrival and wakeup; the standard
epoch tests retain coverage for global, threaded and prefix-stop behavior.

SPM boot transfers use the timed shared-RAM DMA controller. There is no separate
aggregate memory-initialization barrier or byte-count timing shortcut. Epoch
arrivals/releases use dedicated controller links with the configured release
delay; they do not compete for ordinary mesh packet bandwidth.
