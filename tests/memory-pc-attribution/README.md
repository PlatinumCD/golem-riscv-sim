# Memory PC Attribution

This test calls one shared memory site from two functions in runtime task 11.

The QEMU memory event carries the guest program counter to SST. The profile
analyzer resolves the instruction and return addresses against `tile0.elf`.
The test passes only when both events have one shared memory PC and two
different, nonzero caller return addresses.

Run the test with this command:

```bash
./tests/memory-pc-attribution/run-test.sh
```
