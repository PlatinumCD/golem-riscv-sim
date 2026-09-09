# Historical Epoch C runner

This runner belongs to an earlier five-gate baseline, including a private-L1
test that is no longer part of the hardware suite.

It is not a current acceptance command: `run-test.sh` still expects
`src/config/epoch-c.env`, while the preserved preset is now in
`src/config/architectures/epoch-c.env`. Moving that path alone would not
restore the removed gates.

For current validation, use:

```bash
bash tests/run-all.sh --suite hardware
```

See the [hardware suite registry](../../../tools/hardware/hardware_suite.py)
for its actual cases and prerequisites.
