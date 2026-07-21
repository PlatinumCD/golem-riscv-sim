# Golem RISC-V simulation

Reproducible AArch64-hosted LLVM/MLIR RISC-V compiler, static MUSL OpenMP
runtime, and a 10x10 (100 CPU) QEMU/OpenMP proof program.

The two source dependencies are pinned Git submodules:

- `third_party/llvm-project`: PlatinumCD LLVM, commit `d5685386e` on
  `golem-analog`.
- `third_party/riscv-gnu-toolchain`: GNU MUSL sysroot/runtime, commit
  `aa35d4554`.

QEMU is deliberately a host package. Linux is a pinned submodule and is built
with `CONFIG_NR_CPUS=128`, which is required because the Debian installer
kernel used in the first proof was limited to 64 CPUs.

On Debian/Ubuntu, install the host prerequisites (package names may vary):

```bash
sudo apt install build-essential cmake ninja-build git autoconf automake bison flex \
  gawk gperf texinfo curl cpio gzip qemu-system-misc opensbi
```

Build every toolchain component using all host cores by default:

```bash
./bootstrap.sh
```

Build the 100-CPU guest, then run QEMU in the foreground:

```bash
./bootstrap.sh run
```

The guest maps OpenMP worker `n` to logical grid cell `(n / 10, n % 10)` and
pins it to Linux CPU `n`. QEMU `virt` supplies 100 SMP CPUs; it does not model
a physical mesh network. The demo requires at least 100 CPUs.

Compile another static RISC-V program:

```bash
./toolchain/golem-clang -static -fopenmp hello.cpp -o hello
```

All generated output is in `build/`, and is excluded from Git. `run` uses the
prebuilt toolchain; after a fresh clone, run `./bootstrap.sh` once first.
