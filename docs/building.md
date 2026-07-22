# Building the Platform v0 environment

## Host requirements

The build is validated on an AArch64 Debian or Ubuntu host. Required tools and
development libraries include:

```bash
sudo apt install build-essential cmake ninja-build git autoconf automake \
  libtool pkg-config python3 python3-dev bison flex gawk \
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
system emulators are disabled because Platform v0 does not use them.

Useful partial builds are:

```bash
./bootstrap.sh llvm
./bootstrap.sh qemu
./bootstrap.sh sst-core
./bootstrap.sh sst
./bootstrap.sh platform
```

## Source preparation

`third_party/` contains pristine pinned submodules. QEMU and SST Elements are
customized only in generated worktrees:

```text
third_party/qemu + components/devices/mittens-nic + patches/qemu
    -> build/sources/qemu

third_party/sst-elements + components/elements/mittens + bridge
    -> build/sources/sst-elements
```

The SST Elements preparation enables only the two elements required by
Platform v0: `merlin` and `mittens`.

Override `GOLEM_BUILD_ROOT` or `GOLEM_INSTALL_ROOT` to place generated output
outside the repository. Those paths must be consistent across the build and
test invocations.
