# Architecture documentation

The maintained machine is composed in [`src/configuration.py`](../src/configuration.py)
and [`src/components/mordred/tiles.py`](../src/components/mordred/tiles.py).

| Question | Read |
|---|---|
| What is in a tile? | [Architecture](architecture.md) and [diagram](diagrams/single-tile.svg) |
| How do guests access memory and arrays? | [Programming interface](platform.md) |
| How is time modeled? | [Timing](timing-model.md) |
| How do I turn cycle profiling on or off? | [Profiling](profiling.md) |
| What RVV behavior is supported? | [Vector architecture](vector-architecture.md) |
| What do the analog instructions do? | [Analog ISA](analog-isa.md) |
| What are the supported defaults? | [Parameters](parameters.md) |
| What changed during source replacement? | [Migration and recovery](migration.md) |

See the [source map](../src/README.md) and [test guide](../tests/README.md) for implementation and validation.
