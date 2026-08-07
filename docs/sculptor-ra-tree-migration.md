# Sculptor RA-Tree Migration

## Purpose

This repository uses the RA-tree deployment path from the pinned
Sculptor-MLIR revision. The former task-graph, island, and core-partition
passes are retired. Do not add new tests that use those passes.

The simulator remains the system model. Sculptor supplies the compiler-side
mapping and the tile-side runtime library.

```text
PyTorch -> Torch-MLIR -> Sculptor RA tree -> mapping plan
        -> logical-tile placement -> one RISC-V ELF per tile
        -> Sculptor runtime in QEMU -> Mittens NIC -> SST mesh
```

## Ownership

| Component | Owner | Function |
|---|---|---|
| Layer conversion, mapping, placement, and tile metadata | Sculptor-MLIR | Create a placed tile deployment. |
| Tile runtime | Sculptor-MLIR runtime source | Run boot and dispatch work. Manage resources and routes. |
| RISC-V runtime archive | This repository cross-build | Compile Sculptor runtime sources for RISC-V. |
| Golem ISA and NIC MMIO | QEMU and the Mittens element | Execute tile instructions and expose transport. |
| Mesh and timing model | SST and Mittens | Model links, routing, contention, and configured devices. |

The archive installed by the normal Sculptor CMake build is an AArch64 host
archive. It must not be linked into a RISC-V tile ELF. The local
`build-scripts/build-runtime.sh` cross-compiles the pinned Sculptor runtime
sources, including `mlir_runtime.cpp`, and installs the RISC-V archive at:

```text
install/runtime/lib/libgolem-runtime.a
```

## RA-Tree Deployment Pipeline

`build-scripts/lower-sculptor-ra-tree.sh` is the shared compiler-facing
entry point. It requires:

```text
SCULPTOR_INPUT_MLIR=<tensor-level MLIR>
SCULPTOR_DEPLOYMENT_DIR=<output directory>
```

The script writes these inspectable stages:

```text
01-canonical.mlir
02-converted.mlir
03-golem.mlir
04-digital-work.mlir
05-ra-tree.mlir
06-mapping-plan.mlir
08-placed.mlir
09-tile-deployment.mlir
```

`09-tile-deployment.mlir` contains the outlined tile modules. Pass it to
`build-scripts/build-sculptor-core-objects.sh` with:

```text
SCULPTOR_DEPLOYMENT_MLIR=<.../09-tile-deployment.mlir>
SCULPTOR_CORE_OBJECT_DIR=<object output directory>
```

The object builder extracts each active tile, materializes its local runtime
graph, optionally plans scratchpad storage, finalizes the tile ABI, lowers the
task code, and compiles one RISC-V object per tile.

## Simulator Boundary

The runtime library does not replace QEMU or SST.

```text
Generated tile tables
        |
        v
Sculptor runtime inside the tile ELF
        |
        v
QEMU RISC-V execution and Golem ISA
        |
        v
Mittens NIC bridge and SST mesh timing
```

A single-tile functional test can run with QEMU alone. A multi-tile test needs
SST because the runtime calls the NIC transport, while SST supplies the mesh
topology, routing, link serialization, and contention.

## Migration Status

The RISC-V runtime build and host runtime test use the Sculptor runtime
sources. The shared per-tile object builder accepts RA-tree deployments.

The single-tile test checks the complete RA-tree compiler and ELF path. The
model-family suite contains 25 non-GPT Sculptor fixtures. Each fixture can use
the native memory backend or the private-L1 memHierarchy backend.

The repository removed tests that used the retired task-graph passes. New
deployment tests must use the shared RA-tree scripts.

## `apply-mapping-plan`

`sculptor-apply-mapping-plan` is a terminal utility pass. It consumes the RA
tree and logical-tile graph. The compiler now diagnoses an invalid attempt to
place tiles after that pass. The shared deployment script intentionally uses:

```text
plan-mapping -> place-logical-tiles -> outline-tile-routines
```

and does not include `apply-mapping-plan`.
