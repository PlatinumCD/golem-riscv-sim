# Architecture documentation

Start with the [project overview](../README.md), then choose the question you
need answered. The default machine executes code from SPM through an instruction
cache; shared main memory is reached by explicit DMA.

| Question | Read |
|---|---|
| What is in a tile, and how are tiles connected? | [System architecture](architecture.md) |
| How does a program boot, use memory, and send data? | [Programming interface](platform.md) |
| How are execution and transfer cycles calculated? | [Timing model](timing-model.md) |
| What does RVV support, and what timing is modeled? | [Vector architecture](vector-architecture.md) |
| How do the analog instructions work? | [Analog ISA](analog-isa.md) |
| What are the exact tile parameter names and defaults? | [Parameter reference](parameters.md) |

For implementation work, use the [source map](../src/README.md).
For validation, use the [test guide](../tests/README.md).
[Diagram sources](diagrams/README.md) are the same drawings used by the overview.
