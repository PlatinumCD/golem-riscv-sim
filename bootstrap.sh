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
  all            initialize, build, and run the two system tests (default)
  build          initialize and build the complete environment
  check          verify host build dependencies
  llvm           initialize and build LLVM/Clang/LLD/MLIR
  qemu           initialize and build QEMU with the Mittens NIC
  sst-core       initialize and build SST Core
  sst            initialize and build SST Core, Merlin, and Mittens
  platform       build all bare-metal test images
  test           run the hello and mesh-pair system tests
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
    "${ROOT}/tests/hello/run-test.sh"
    "${ROOT}/tests/mesh-pair/run-test.sh"
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
    platform)
        "${ROOT}/build-scripts/build-platform.sh" all
        ;;
    test)
        run_system_tests
        ;;
    test-elements)
        "${ROOT}/components/elements/mittens/tests/run-test.sh"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
