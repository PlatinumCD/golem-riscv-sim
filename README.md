# Golem RISC-V simulation

Reproducible AArch64-hosted LLVM/MLIR RISC-V compiler, static MUSL OpenMP
runtime, and a five-CPU QEMU proof program.

The two source dependencies are pinned Git submodules:

- `third_party/llvm-project`: PlatinumCD LLVM, commit `d5685386e` on
  `golem-analog`.
- `third_party/riscv-gnu-toolchain`: GNU MUSL sysroot/runtime, commit
  `aa35d4554`.

QEMU is deliberately a host package and the kernel is fetched from Debian's
RISC-V installer image. This keeps the source checkout focused on the two
toolchain repositories while retaining a one-command runnable demonstration.

On Debian/Ubuntu, install the host prerequisites (package names may vary):

```bash
sudo apt install build-essential cmake ninja-build git autoconf automake bison flex \
  gawk gperf texinfo curl cpio gzip qemu-system-misc opensbi
```

Build every toolchain component using all host cores by default:

```bash
./bootstrap.sh
```

Build the five-core guest, then run QEMU in the foreground:

```bash
./bootstrap.sh run
```

Compile another static RISC-V program:

```bash
./toolchain/golem-clang -static -fopenmp hello.cpp -o hello
```

All generated output is in `build/`, and is excluded from Git. `run` uses the
prebuilt toolchain; after a fresh clone, run `./bootstrap.sh` once first.
