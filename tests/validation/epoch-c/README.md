# Epoch C component validation

This foreground-only runner executes the five evidence gates required by the
frozen Epoch C baseline:

1. scalar/RVV CPU throughput and synchronization-quantum invariance;
2. mesh serialization, distance, and contention;
3. end-to-end analog command and shared-link timing;
4. controlled private-L1 timing; and
5. deployment-transmit synchronization and backpressure.

It does not build or run the GPT-2 scheduling sweep.

```bash
./tests/validation/epoch-c/run-test.sh
```
