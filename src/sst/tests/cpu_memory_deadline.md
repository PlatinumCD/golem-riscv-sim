# R5b: CPU delivery and SPM-service deadlines

This standalone correctness oracle is deliberately outside the exact-reference
comparison case list. It uses no L1/L2/cache, NIC, analog device, or global DMA.
It builds only two small guest ELFs; it never builds or installs SST or QEMU.

```
python3 src2/sst/tests/cpu_memory_deadline.py \
  --reference-install build/src2/baselines/r8-1788563110972463702/install
```

Both arms execute the same guest bytes and parameters. Each run has isolated
artifacts and records fixture, guest, and before/after binary hashes. Exit 1
means the required arm violates the correctness oracle; exit 2 means invalid
evidence. By default src2 is required; `--reference-only` requires the reference
and is intended to fail before correction, not reinterpret the bug as success.

The ordinary path is StandardMem -> MemController/simpleMem, noncacheable and
timing-only. Functional values still come from QEMU. After initialization:

* Delivery: one buffered ordinary store, 4096 register-only additions, fence,
  task-finish marker. The marker must not precede even those 4096 CPU cycles.
  The fence is intentionally not inferred from waits.csv (fences are unprofiled).
* SPM service: one buffered ordinary store, two adjacent SPM loads. Each load
  must honor its 10000-CPU-cycle service interval. The second load exposes a
  leftover first-load wake incorrectly completing a new access.

Clocks: 500 MHz / 1 GHz / 2 GHz; explicit SST timebase 1 ps. Store-buffer depths
1 (blocking control) and 2 (overlap). Ordinary RAM latency is 100 ns. The oracle
requires evidence that the response interrupts the intended delay in overlap
cases, as well as payload success. It never accepts a failed guest as timing
evidence. Deadline ticks = origin ticks + duration CPU cycles * CPU tick factor.

Before-production-edit evidence:

* `build/src2/r5b-before-controller/delivery.log`: real CpuExecutionController,
  fake host, response tick 10 resumes fence before deadline 82 (FAIL).
* `build/src2/cpu-memory-deadline-lvo6xiim/results.json`: pinned R8, six overlap
  failures, six blocking-control passes, all payload passes, binaries unchanged.
  At 1 GHz the SPM loads retire at 150000 / 10048000, before their respective
  deadlines 10048000 / 10151000 ticks. Delivery marker is at 156000 instead of
  at least 4143000 ticks.

The controller's default host test also exercises premature process retries,
explicit memory/analog completions, service delays, revocable timer generations,
and stopped-controller wakes at four tick factors. These supplement rather than
replace the integrated cacheless oracle.

R5b is a separately validated timing correction, not timing-equivalent owner
extraction. No expectations in existing compatibility fixtures are changed.
