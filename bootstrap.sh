#!/usr/bin/env bash
# Reproduce the complete bare-metal Golem tile compiler and simulator.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ACTION="${1:-all}"
# shellcheck source=build-scripts/common.sh
source "${ROOT}/build-scripts/common.sh"

required_submodules=(
    third_party/llvm-project
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
  llvm           initialize and build LLVM/Clang/LLD/MLIR
  qemu           initialize and build QEMU with the Mittens NIC
  sst-core       initialize and build SST Core
  sst            initialize and build SST Core, Merlin, and Mittens
  runtime        build and install the bare-metal runtime archive
  platform       build all bare-metal test images
  test           run all boot, communication, routing, and compute proofs
  test-runtime   run host and RISC-V QEMU runtime-library proofs
  test-elements  run the complete Mittens element test directory
EOF
}

initialize_submodules() {
    git -C "${ROOT}" submodule sync -- "$@"
    git -C "${ROOT}" submodule update --init --jobs "${BUILD_JOBS}" -- "$@"
}

build_environment() {
    "${ROOT}/build-scripts/build-llvm.sh"
    "${ROOT}/build-scripts/build-qemu.sh"
    "${ROOT}/build-scripts/build-sst-core.sh"
    "${ROOT}/build-scripts/build-sst-elements.sh"
    "${ROOT}/build-scripts/build-platform.sh" all
}

run_system_tests() {
    "${ROOT}/runtime/tests/run-test.sh"
    "${ROOT}/tests/runtime-library/run-test.sh"
    "${ROOT}/tests/hello/run-test.sh"
    "${ROOT}/tests/mesh-pair/run-test.sh"
    "${ROOT}/tests/mesh-3x3/run-test.sh"
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
    llvm)
        initialize_submodules third_party/llvm-project
        "${ROOT}/build-scripts/build-llvm.sh"
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
        initialize_submodules third_party/sst-core third_party/sst-elements
        "${ROOT}/build-scripts/build-sst-core.sh"
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
        "${ROOT}/runtime/tests/run-test.sh"
        "${ROOT}/tests/runtime-library/run-test.sh"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
