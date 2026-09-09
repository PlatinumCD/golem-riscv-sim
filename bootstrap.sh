#!/usr/bin/env bash
# Reproduce the complete bare-metal Golem tile compiler and simulator.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ACTION="${1:-hardware}"
# shellcheck source=build-scripts/common.sh
source "${ROOT}/build-scripts/common.sh"

required_submodules=(
    third_party/llvm-project
    third_party/torch-mlir
    third_party/sculptor-mlir
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
  all            explicitly initialize/build the full environment and run all suites
  build          initialize and build the complete environment
  check          verify host build dependencies
  compiler-python install the pinned local PyTorch compiler environment
  riscv-gnu-toolchain initialize and install the bare-metal RISC-V GNU toolchain
  llvm           initialize and build LLVM/Clang/LLD/MLIR
  torch-mlir     initialize and build LLVM/MLIR and Torch-MLIR
  sculptor-mlir  initialize and build LLVM/MLIR and Sculptor-MLIR
  crosssim       initialize and install the minimal CrossSim Python stack
  qemu           initialize and build QEMU with the Mittens NIC and analog ISA
  sst-core       initialize and build SST Core
  sst            initialize and build SST Core, memHierarchy, Merlin, and Mittens
  runtime        build and install the bare-metal runtime archive
  platform       build all bare-metal test images
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
    "${ROOT}/build-scripts/build-llvm.sh"
    "${ROOT}/build-scripts/build-torch-mlir.sh"
    "${ROOT}/build-scripts/build-sculptor-mlir.sh"
    "${ROOT}/build-scripts/build-qemu.sh"
    "${ROOT}/build-scripts/build-sst-core.sh"
    "${ROOT}/build-scripts/build-cross-sim.sh"
    "${ROOT}/build-scripts/build-sst-elements.sh"
    "${ROOT}/build-scripts/build-platform.sh" all
}

run_system_tests() {
    bash "${ROOT}/tests/run-all.sh" --suite all
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
        run_system_tests
        ;;
    build)
        "${ROOT}/build-scripts/check-dependencies.sh"
        initialize_submodules "${required_submodules[@]}"
        build_environment
        ;;
    check)
        "${ROOT}/build-scripts/check-dependencies.sh"
        ;;
    compiler-python)
        "${ROOT}/build-scripts/build-compiler-python.sh"
        ;;
    riscv-gnu-toolchain|gnu-riscv-toolchain)
        "${ROOT}/build-scripts/build-riscv-gnu-toolchain.sh"
        ;;
    llvm)
        initialize_submodules third_party/llvm-project
        "${ROOT}/build-scripts/build-llvm.sh"
        ;;
    torch-mlir)
        initialize_submodules third_party/llvm-project third_party/torch-mlir
        "${ROOT}/build-scripts/build-llvm.sh"
        "${ROOT}/build-scripts/build-torch-mlir.sh"
        ;;
    sculptor-mlir)
        initialize_submodules third_party/llvm-project \
            third_party/sculptor-mlir
        "${ROOT}/build-scripts/build-llvm.sh"
        "${ROOT}/build-scripts/build-sculptor-mlir.sh"
        ;;
    crosssim)
        initialize_submodules third_party/cross-sim
        "${ROOT}/build-scripts/build-cross-sim.sh"
        ;;
    qemu)
        initialize_submodules third_party/qemu
        "${ROOT}/build-scripts/build-qemu.sh"
        ;;
    sst-core)
        initialize_submodules third_party/sst-core
        "${ROOT}/build-scripts/build-sst-core.sh"
        ;;
    sst)
        initialize_submodules third_party/sst-core third_party/sst-elements \
            third_party/cross-sim
        "${ROOT}/build-scripts/build-sst-core.sh"
        "${ROOT}/build-scripts/build-cross-sim.sh"
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
