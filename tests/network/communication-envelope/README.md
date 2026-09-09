# Communication envelope wrapper

This entry point delegates to the local-only communication-envelope study in
`studies/compute-communication/communication-envelope/`. It does not contain
a self-contained hardware test.

The study's regression selection covers delayed receive, fan-out, shared-link
traffic, duplex traffic, concurrent RVV/DMA, bank placement, transfer tails,
and queue backpressure.

```bash
bash tests/run-all.sh --case network/communication-envelope
```

**Requires the local studies tree.** That tree is ignored by Git, but this
wrapper is still registered in the hardware suite. A checkout without it
cannot execute this case. Keeping the regression independent of the studies
tree requires a separate code change.

Passing transfer and accounting checks does not establish peak client rates.
