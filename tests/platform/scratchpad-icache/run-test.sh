#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_DIR}/../../support/test-env.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${QEMU_SYSTEM_RISCV64:-${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64}"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUT="${TEST_RESULTS_ROOT}/scratchpad-icache"

require_executable "${SST}"
require_executable "${QEMU}"
require_file "${ELEMENT_LIBRARY}/libmittens.so"
make -C "${TEST_DIR}" OUT="${OUT}/guest" all
mkdir -p -- "${OUT}"

warm_lines=()
for quantum in 1 7 100; do
    profile="${OUT}/warm-loop/quantum-${quantum}/profile"
    mkdir -p -- "${profile}"
    log="${OUT}/warm-loop/quantum-${quantum}/simulation.log"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_TEST_ELF="${OUT}/guest/warm-loop.elf" \
    MITTENS_TEST_PROFILE="${profile}" MITTENS_TEST_QUANTUM="${quantum}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
      timeout --foreground 60s "${SST}" "${TEST_DIR}/simulation.py" > "${log}" 2>&1
    grep -q "QEMU tile 0 exited with exit status 0" "${log}"
    grep -q "SCRATCHPAD_ICACHE_PASS" "${log}"
    grep -Eq "SCRATCHPAD_BOOT tile=0 bytes=[0-9]+ cycles=[0-9]+" "${log}"
    warm_lines+=("$(${TEST_DIR}/check-summary.py "${log}" 0 0 0)")
done
for line in "${warm_lines[@]}"; do
    [[ "${line}" == "${warm_lines[0]}" ]] || {
        echo "warm-loop cache counters changed with instruction quantum" >&2
        exit 1
    }
done
python3 "${TEST_DIR}/compare-profile.py" \
    "${OUT}/warm-loop/quantum-1/profile" \
    "${OUT}/warm-loop/quantum-7/profile" \
    "${OUT}/warm-loop/quantum-100/profile"

for test in warm-loop eviction straddle fence-i trap-ram rvv-spm rvv-fault reload-code dma-code; do
    [[ "${test}" == warm-loop ]] && continue
    profile="${OUT}/${test}/profile"
    mkdir -p -- "${profile}"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_TEST_ELF="${OUT}/guest/${test}.elf" \
    MITTENS_TEST_PROFILE="${profile}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
      timeout --foreground 60s "${SST}" "${TEST_DIR}/simulation.py" > "${OUT}/${test}/simulation.log" 2>&1
    grep -q "QEMU tile 0 exited with exit status 0" "${OUT}/${test}/simulation.log"
    grep -q "SCRATCHPAD_ICACHE_PASS" "${OUT}/${test}/simulation.log"
    grep -Eq "SCRATCHPAD_BOOT tile=0 bytes=[0-9]+ cycles=[0-9]+" "${OUT}/${test}/simulation.log"
    case "${test}" in
        eviction) "${TEST_DIR}/check-summary.py" "${OUT}/${test}/simulation.log" 513 0 0 ;;
        fence-i|reload-code|dma-code) "${TEST_DIR}/check-summary.py" "${OUT}/${test}/simulation.log" 0 1 0 ;;
        trap-ram|rvv-fault) "${TEST_DIR}/check-summary.py" "${OUT}/${test}/simulation.log" 0 0 1 ;;
        rvv-spm)
            "${TEST_DIR}/check-summary.py" "${OUT}/${test}/simulation.log" 0 0 0
            python3 "${TEST_DIR}/check-vector.py" "${profile}" \
                "${OUT}/guest/rvv-spm.elf" "${GOLEM_LLVM_DIR}/bin/llvm-nm"
            ;;
        *) "${TEST_DIR}/check-summary.py" "${OUT}/${test}/simulation.log" 0 0 0 ;;
    esac
done
for vector_test in rvv-spm rvv-fault; do
  for quantum in 7 100; do
    profile="${OUT}/${vector_test}/quantum-${quantum}/profile"
    mkdir -p -- "${profile}"
    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_TEST_ELF="${OUT}/guest/${vector_test}.elf" \
    MITTENS_TEST_PROFILE="${profile}" MITTENS_TEST_QUANTUM="${quantum}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        timeout --foreground 60s "${SST}" "${TEST_DIR}/simulation.py" \
        > "${OUT}/${vector_test}/quantum-${quantum}/simulation.log" 2>&1
    grep -q 'SCRATCHPAD_ICACHE_PASS' "${OUT}/${vector_test}/quantum-${quantum}/simulation.log"
    if [[ "${vector_test}" == rvv-spm ]]; then
        python3 "${TEST_DIR}/check-vector.py" "${profile}" \
            "${OUT}/guest/rvv-spm.elf" "${GOLEM_LLVM_DIR}/bin/llvm-nm"
    else
        "${TEST_DIR}/check-summary.py" "${OUT}/${vector_test}/quantum-${quantum}/simulation.log" 0 0 1
    fi
  done
  python3 "${TEST_DIR}/compare-profile.py" \
      "${OUT}/${vector_test}/profile" "${OUT}/${vector_test}/quantum-7/profile" \
      "${OUT}/${vector_test}/quantum-100/profile"
done
echo "scratchpad/icache hardware fixtures: PASS"
