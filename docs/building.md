# Building the Platform v0.1 environment

## Host requirements

The build is validated on an AArch64 Debian or Ubuntu host. Required tools and
development libraries include:

```bash
sudo apt install build-essential cmake ninja-build git autoconf automake \
  libtool pkg-config python3 python3-dev python3-pip python3-venv bison flex gawk \
  libglib2.0-dev libpixman-1-dev libopenmpi-dev
```

Package names may differ on other distributions. `./bootstrap.sh check`
validates command-line tools before starting a long build; individual
configure steps report missing development libraries.

QEMU uses `/usr/bin/python3` by default to avoid incompatible user-local
Python environments. Set `QEMU_PYTHON` when a different interpreter provides
the required host modules.

## Complete build

```bash
./bootstrap.sh build
```

By default, builds use every online host CPU. Set `JOBS` to cap parallelism.
Generated files are confined to `build/` and `install/`.

The QEMU build is deliberately minimal: it installs only
`qemu-system-riscv64`. Guest tools, the guest agent, plugins, SLiRP, and other
system emulators are disabled because Platform v0.1 does not use them.

Useful partial builds are:

```bash
./bootstrap.sh compiler-python
./bootstrap.sh llvm
./bootstrap.sh torch-mlir
./bootstrap.sh sculptor-mlir
./bootstrap.sh crosssim
./bootstrap.sh qemu
./bootstrap.sh sst-core
./bootstrap.sh sst
./bootstrap.sh runtime
./bootstrap.sh platform
```

`./bootstrap.sh compiler-python` creates `install/compiler-python` and installs
the pinned CPU-only PyTorch package and the Python build dependencies required
by the MLIR bindings. It does not install CUDA, torchvision, or a training
environment. The LLVM and Torch-MLIR builds use this interpreter explicitly,
so a user-local Python environment cannot change their extension ABI.

`./bootstrap.sh runtime` compiles the freestanding runtime with the pinned
Golem Clang and installs:

```text
install/runtime/include/golem/runtime/
install/runtime/lib/libgolem-runtime.a
```

Every platform image links this archive statically. Running
`build-scripts/build-platform.sh` also refreshes the archive, so individual
test builds cannot accidentally use stale runtime objects.

`./bootstrap.sh torch-mlir` builds the pinned `analog-extension` source as a
standalone MLIR project against `install/llvm`. It does not initialize or use
Torch-MLIR's nested `externals/llvm-project`; the Golem LLVM and MLIR packages
are passed explicitly through `LLVM_DIR` and `MLIR_DIR`. The build enables the
MLIR and Torch-MLIR Python bindings needed by the pure-Python
`torch.export`/FX importer. StableHLO, Torch's deprecated JIT importer, native
PyTorch extensions, and both nested Torch-MLIR submodules remain disabled.
The preparation step applies one project patch in
`build/sources/torch-mlir` so the standalone build respects the installed
MLIR package's binding and test settings.

`./bootstrap.sh sculptor-mlir` builds the pinned out-of-tree Sculptor compiler
against the same installed Golem LLVM/MLIR packages. Sculptor is a host tool,
so its C++ sources use the native AArch64 compiler while its dialects and
passes link against the project LLVM/MLIR installation. The resulting driver
is `install/sculptor-mlir/bin/sculptor-mlir-opt`.

`./bootstrap.sh crosssim` installs the pinned CPU-only CrossSim 3.2.1 stack
under `install/cross-sim/python`. It installs only CrossSim, NumPy, and SciPy
for the Python 3.12 interpreter used by SST. CuPy, CUDA support, DNN
frameworks, tutorials, datasets, and pretrained models are not required.
When launching SST directly with `analog_backend=crosssim`, expose that local
installation to SST's embedded interpreter:

```bash
export PYTHONPATH="$PWD/install/cross-sim/python${PYTHONPATH:+:$PYTHONPATH}"
```

## Source preparation

`third_party/` contains pristine pinned submodules. QEMU and SST Elements are
customized only in generated worktrees:

```text
third_party/qemu + components/devices + components/qemu + patches/qemu
    -> build/sources/qemu

third_party/sst-elements + components/elements/mittens + bridge
    -> build/sources/sst-elements

third_party/cross-sim
    -> build/sources/cross-sim
    -> install/cross-sim/python
```

The SST Elements preparation enables the three elements required by
Platform v0.1: `memHierarchy`, `merlin`, and `mittens`.

Override `GOLEM_BUILD_ROOT` or `GOLEM_INSTALL_ROOT` to place generated output
outside the repository. Those paths must be consistent across the build and
test invocations.
