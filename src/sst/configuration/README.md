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

There is one execution architecture: code/data/stack in SPM, timed instruction
fetches through the I-cache, and explicit shared-memory DMA. Managed tiles need
a `globalDMA` connection for boot; their ELF layout must fit the configured SPM.
The [mesh builder](../../../tests/support/mesh.py) wires that controller and the
wormhole routers. SPM geometry, cache geometry, links, lanes and analog resources
remain configurable. Unsupported execution modes fail before guest launch.

The [parameter reference](../../../docs/parameters.md) lists the same settings
as readable tables. After editing defaults or descriptions, regenerate it with
`python3 tools/hardware/parameter-reference.py`; `--check` verifies it is current.

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
