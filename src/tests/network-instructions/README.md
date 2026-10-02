# Guest network instruction checks

Actual QEMU guests submit whole messages through their NIUs, Mordred XY routers
and timed banked SPM. No external controller launches transfers. See the
[instruction contract](../../components/mordred/network-instructions.md).

```sh
python3 -B src/components/riscv-qemu/build_qemu.py
python3 -B src/tests/network-instructions/test_configuration.py
python3 -B src/tests/network-instructions/run.py
python3 -B src/tests/network-instructions/run.py --case mvm-multi-hop-mvm
bash tests/run-all.sh --case network/guest-instructions
```

Deployment records and `deployment.h` are generated together by the host fixture.
Guests submit the supplied compiler identities unchanged; descriptors stay 32 bytes.

| Contract | Check |
|---|---|
| Final-destination routing | Multi-hop transfers in both directions; exact router output flits and zero intermediate NIU/SPM/CPU forwarding |
| Source-agnostic receive | Two producers target one destination using operand-free receive |
| Reserved inputs | B arrives first, B's next invocation waits, A arrives while the first B buffer stays held |
| Identity | Distinct transfer and invocation IDs, sequence ordering, zero and maximum 63-bit IDs; invalid values rejected |
| Source ownership | Overwrite after wait but before destination consumption; exact received bytes |
| Receive ownership | Re-read held buffer, stable metadata, stale tokens and double release |
| Progress | Command/ticket-full admission, single-slot backpressure, independent transfers, hardware credit return |
| Analog integration | MVM at tile 0 → XY through tile 1 → MVM at tile 3, with exact output and unrelated analog work during wait |

Other cases cover 64 B, 256 B, 4 KiB and 64 KiB payloads, out-of-order completion
with ordered acquisition, descriptor reuse, 16-byte packets, exact NIC capacity,
512-bit flit rounding, a single packet slot and memory fragment, shared-bank
permission rejection, multiple VCs, LSQ
depth 1, bidirectional traffic, successful try-receive, delayed credits after CPU exit, instruction-budget/profiling invariance, and illegal
setup or nonzero receive-register encodings. LLVM and GNU assembly encodings are
checked when those compilers are installed.

`validation.json` and per-case traces record the evidence. Copies in `guest.c`
verify received values; they are application consumers, never message transport.
The MVM case consumes the received SPM pointer directly. These are correctness
tests, not network throughput studies.

Small, Medium and Large cases load the deployable settings in
[`tile_profiles.json`](../../tile_profiles.json): **1 MiB, 1.5 MiB and 2 MiB per tile**.
Each runs source reuse, two producers with reserved receive inputs, and a
32×32 MVM → multi-hop transfer → MVM with one array per tile. The original
independent-array progress test retains two arrays. The fixture places all
payloads and verification buffers below 1 MiB; its linker and stack use the
configured capacity. LLVM also targets the selected VLEN explicitly, so descriptor
initialization cannot assume registers wider than the simulated CPU. Every guest writes the final SPM word, and validation
checks it and the exact backing-file length. Host checks reject receive slots
past each capacity, including the non-power-of-two Medium configuration.
