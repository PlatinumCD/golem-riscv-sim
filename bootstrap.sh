#!/usr/bin/env bash
# Reproduce the complete bare-metal Golem tile compiler and simulator.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ACTION="${1:-hardware}"
# shellcheck source=build-scripts/common.sh
source "${ROOT}/build-scripts/common.sh"

required_submodules=(
    third_party/llvm-project
    third_party/cross-sim
    third_party/qemu
    third_party/sst-core
    third_party/sst-elements
)

usage() {
    cat <<'EOF'
usage: ./bootstrap.sh [action]

actions:
  hardware       rebuild QEMU/Mittens and run hardware tests using existing dependencies (default)
  build-hardware rebuild QEMU/Mittens using existing dependencies
  test-hardware  run the hardware correctness suite only
  all            initialize/build hardware dependencies and hardware, then run hardware tests
  build          initialize/build hardware dependencies and hardware
  dependencies   initialize/build shared dependencies in build/ and install/
  compiler       build optional compiler (requires GOLEM_SCULPTOR_SOURCE)
  check          verify host build dependencies
  compiler-python install the pinned local PyTorch compiler environment
  riscv-gnu-toolchain initialize and install the bare-metal RISC-V GNU toolchain
  llvm           initialize and build LLVM/Clang/LLD/MLIR
  torch-mlir     initialize and build LLVM/MLIR and Torch-MLIR
  sculptor-mlir  alias for compiler; requires GOLEM_SCULPTOR_SOURCE
  crosssim       initialize and install the minimal CrossSim Python stack
  qemu           initialize and build QEMU with the Mittens NIC and analog ISA
  sst-core       initialize and build SST Core
  sst            initialize and build SST Core, memHierarchy, Merlin, and Mittens
  runtime        build optional Sculptor runtime; requires GOLEM_SCULPTOR_SOURCE
  platform       build optional compiler test images; requires GOLEM_SCULPTOR_SOURCE
  test           run the hardware correctness suite (same as test-hardware)
  test-runtime   run host, QEMU, and QEMU/SST runtime proofs
  test-elements  run the complete Mittens element test directory
EOF
}

initialize_submodules() {
    git -C "${ROOT}" submodule sync -- "$@"
    git -C "${ROOT}" submodule update --init --jobs "${BUILD_JOBS}" -- "$@"
}

build_environment() {
    for dependency in riscv-gnu-toolchain llvm sst-core cross-sim sst-elements; do
        shared_build "${dependency}"
    done
}

shared_build() {
    GOLEM_BUILD_SCOPE=shared "${ROOT}/build-scripts/build-$1.sh"
}

run_system_tests() {
    bash "${ROOT}/tests/run-all.sh" --suite hardware
}

case "${ACTION}" in
    hardware|build-hardware)
        python3 "${ROOT}/tools/hardware/build.py" all -j "${BUILD_JOBS}"
        if [[ "${ACTION}" == hardware ]]; then
            bash "${ROOT}/tests/run-all.sh" --suite hardware
        fi
        ;;
    all)
        "${ROOT}/build-scripts/check-dependencies.sh"
        initialize_submodules "${required_submodules[@]}"
        build_environment
        python3 "${ROOT}/tools/hardware/build.py" all -j "${BUILD_JOBS}"
        run_system_tests
        ;;
    build|dependencies)
        "${ROOT}/build-scripts/check-dependencies.sh"
        initialize_submodules "${required_submodules[@]}"
        build_environment
        if [[ "${ACTION}" == build ]]; then
            python3 "${ROOT}/tools/hardware/build.py" all -j "${BUILD_JOBS}"
        fi
        ;;
    check)
        "${ROOT}/build-scripts/check-dependencies.sh"
        ;;
    compiler-python)
        "${ROOT}/build-scripts/build-compiler-python.sh"
        ;;
    riscv-gnu-toolchain|gnu-riscv-toolchain)
        shared_build riscv-gnu-toolchain
        ;;
    llvm)
        initialize_submodules third_party/llvm-project
        shared_build riscv-gnu-toolchain
        shared_build llvm
        ;;
    torch-mlir)
        initialize_submodules third_party/llvm-project third_party/torch-mlir
        shared_build riscv-gnu-toolchain
        shared_build llvm
        shared_build torch-mlir
        ;;
    sculptor-mlir|compiler)
        require_sculptor_source
        initialize_submodules third_party/llvm-project third_party/torch-mlir
        shared_build riscv-gnu-toolchain
        shared_build llvm
        shared_build torch-mlir
        shared_build sculptor-mlir
        ;;
    crosssim)
        initialize_submodules third_party/cross-sim
        shared_build cross-sim
        ;;
    qemu)
        initialize_submodules third_party/qemu
        "${ROOT}/build-scripts/build-qemu.sh"
        ;;
    sst-core)
        initialize_submodules third_party/sst-core
        shared_build sst-core
        ;;
    sst)
        initialize_submodules third_party/sst-core third_party/sst-elements \
            third_party/cross-sim
        shared_build sst-core
        shared_build cross-sim
        shared_build sst-elements
        "${ROOT}/build-scripts/build-sst-elements.sh"
        ;;
    runtime)
        "${ROOT}/build-scripts/build-runtime.sh"
        ;;
    platform)
        "${ROOT}/build-scripts/build-platform.sh" all
        ;;
    test|test-hardware)
        bash "${ROOT}/tests/run-all.sh" --suite hardware
        ;;
    test-elements)
        bash "${ROOT}/tests/run-all.sh" --suite hardware --case component
        ;;
    test-runtime)
        "${ROOT}/tests/runtime/library/run-test.sh"
        "${ROOT}/tests/runtime/deployment-pair/run-test.sh"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
