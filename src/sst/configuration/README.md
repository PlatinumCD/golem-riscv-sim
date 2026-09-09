# Machine configuration

This directory reads and validates settings before resource execution.

| Owner | Settings |
|---|---|
| TileConfiguration | CPU/RVV, scratchpad, DMA, analog, guest launch, and tracing |
| RouterConfiguration | Mesh geometry, links, local lanes, buffers, and pipeline |
| NetworkInterfaceConfiguration | Local network interface and buffering |
| GlobalRAMConfiguration | Capacity, channels, queues, and dependency handling |

[tileParameters.h](tileParameters.h) defines tile parameter names, types,
defaults, and documentation. Controller state and scheduling queues do not
belong in configuration objects.

Set `GOLEM_RESOLVED_CONFIG_DIR` to write per-instance JSON snapshots.
[env.sh](../../../tools/hardware/env.sh) sets a default when used to launch a
command. Snapshot filenames distinguish processes and instances.

TX and RX lane counts are independent. RX queue depth is not RX lane count.
Neither setting duplicates SPM ports or cardinal network links.

From the repository root:

```bash
bash tests/run-all.sh --case configuration
```

The rejection tests check the expected diagnostic, not just a nonzero exit.
