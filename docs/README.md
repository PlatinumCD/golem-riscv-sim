# Golem architecture

These documents describe implemented components. Deployment builders can override
component defaults.

| Document | Scope |
|---|---|
| [System architecture](architecture.md) | Components, ownership, and data paths |
| [Platform](platform.md) | Guest devices, scratchpad, DMA, and communication |
| [Vector architecture](vector-architecture.md) | RVV geometry, compiler target, and timing limits |
| [Analog ISA](analog-isa.md) | Custom matrix-compute instructions |
| [Timing model](timing-model.md) | CPU, memory, DMA, mesh, and analog timing |
| [Parameters](parameters.md) | Generated parameter reference |
| [Diagrams](diagrams/README.md) | Current tile and mesh boundaries |

Start with [source ownership](../src/README.md),
[tile parameters](../src/sst/configuration/tileParameters.h), and
[network configuration](../src/sst/configuration/networkConfiguration.cc).
The [bridge headers](../src/bridge/include/mittens/) define transport protocols;
their versions are independent of ISA versions.

Documentation describes current interfaces and limits, not implementation history.
Defaults come from source; experiment settings must be identified separately.
Resource service and stall counters are not elapsed runtime and must not be
summed as if they were sequential.
