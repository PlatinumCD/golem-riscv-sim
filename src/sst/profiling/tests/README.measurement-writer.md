# Measurement writer tests

These host tests check summary serialization and validation. They do not run
SST or measure hardware performance.

[measurement_writer_test.cpp](measurement_writer_test.cpp) exercises
`MeasurementWriter` and the `PerformanceProfile` summary API. The CSV fixture
in [expected_summary_csv.h](expected_summary_csv.h) is independent of the
writer under test.

## Coverage

- CSV compatibility and JSON parsing.
- Missing, disabled, not-measured, and measured-zero values.
- Integer limits, escaping, locale independence, and provenance.
- Completion/submission consistency.
- TX observation histograms and their declared observation window.
- Validation errors before output mutation.

## Run

From the repository root:

```bash
python3 -B tools/hardware/verify.py
```

This includes the writer and profile tests alongside other host checks. It
requires a C++17 compiler and the vendored JSON header in SST Core.

## Reading the output

JSON includes availability, unit, clock domain, and aggregation metadata.
Legacy CSV has no availability field: a zero or compatibility value there
does not establish that a counter was measured.

TX observation histograms describe sampled controller state over an observation
window. They are not an exact link-busy trace. Shared SPM service includes DMA;
do not add it again as a separate elapsed-time cost.

See [the measurement contract](../MEASUREMENT_CONTRACT.md).
