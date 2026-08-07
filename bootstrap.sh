#!/usr/bin/env bash
# Reproduce the complete bare-metal Golem tile compiler and simulator.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ACTION="${1:-all}"
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
  all            initialize, build, and run all system tests (default)
  build          initialize and build the complete environment
  check          verify host build dependencies
  compiler-python install the pinned local PyTorch compiler environment
  llvm           initialize and build LLVM/Clang/LLD/MLIR
  torch-mlir     initialize and build LLVM/MLIR and Torch-MLIR
  sculptor-mlir  initialize and build LLVM/MLIR and Sculptor-MLIR
  crosssim       initialize and install the minimal CrossSim Python stack
  qemu           initialize and build QEMU with the Mittens NIC and analog ISA
  sst-core       initialize and build SST Core
  sst            initialize and build SST Core, memHierarchy, Merlin, and Mittens
  runtime        build and install the bare-metal runtime archive
  platform       build all bare-metal test images
  test           run compiler, element, boot, mesh, routing, and compute proofs
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
    "${ROOT}/components/elements/mittens/tests/run-test.sh"
    "${ROOT}/visualizer/tests/run-test.sh"
    "${ROOT}/tests/torch-mlir/run-test.sh"
    "${ROOT}/tests/pytorch-single-core/run-test.sh"
    "${ROOT}/tests/sculptor-ra-tree-single-tile/run-test.sh"
    "${ROOT}/tests/runtime-library/run-test.sh"
    "${ROOT}/tests/memory-hierarchy-l1/run-test.sh"
    "${ROOT}/tests/deployment-runtime-pair/run-test.sh"
    "${ROOT}/tests/hello/run-test.sh"
    "${ROOT}/tests/riscv-vector/run-test.sh"
    "${ROOT}/tests/cpu-timing-validation/run-test.sh"
    "${ROOT}/tests/analog-instructions/run-test.sh"
    "${ROOT}/tests/analog-ops/run-test.sh"
    "${ROOT}/tests/analog-timing-validation/run-test.sh"
    "${ROOT}/tests/analog-mesh-2x2/run-test.sh"
    "${ROOT}/tests/analog-route-2x2/run-test.sh"
    "${ROOT}/tests/analog-mesh-2x2-dual-array/run-test.sh"
    "${ROOT}/tests/mesh-pair/run-test.sh"
    "${ROOT}/tests/mesh-3x3/run-test.sh"
    "${ROOT}/tests/network-timing-validation/run-test.sh"
    "${ROOT}/tests/transmit-fanout/run-test.sh"
    "${ROOT}/tests/memory-timing-validation/run-test.sh"
    "${ROOT}/tests/mesh-pipeline/run-test.sh"
    "${ROOT}/tests/distributed-matvec/run-test.sh"
}

case "${ACTION}" in
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
    test)
        run_system_tests
        ;;
    test-elements)
        "${ROOT}/components/elements/mittens/tests/run-test.sh"
        ;;
    test-runtime)
        "${ROOT}/tests/runtime-library/run-test.sh"
        "${ROOT}/tests/deployment-runtime-pair/run-test.sh"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
