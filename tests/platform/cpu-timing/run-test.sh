#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly ELF="${BUILD_ROOT}/tests/cpu-timing-validation/cpu-timing-validation.elf"
readonly OUTPUT_ROOT="${BUILD_ROOT}/tests/cpu-timing-validation"
readonly RESULTS="${OUTPUT_ROOT}/results.csv"
readonly ANALYZER="${TEST_DIR}/analyze-results.py"

require_executable "${SST}"
require_executable "${QEMU}"
require_executable "${ANALYZER}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" cpu-timing-validation

trial_specs=()
for width in 1 2 4; do
    for quantum in 37 1000; do
        trial="${OUTPUT_ROOT}/width-${width}/quantum-${quantum}"
        profile="${trial}/profile"
        tasks="${trial}/tasks"
        log="${trial}/simulation.log"

        mkdir -p -- "${profile}" "${tasks}"
        rm -f -- "${profile}/tile-0-summary.csv" \
            "${tasks}/tile-0.csv" \
            "${log}"

        printf '[cpu timing] issue_width=%s instruction_quantum=%s\n' \
            "${width}" "${quantum}"
        MITTENS_TEST_QEMU="${QEMU}" \
        MITTENS_TEST_ELF="${ELF}" \
        MITTENS_CPU_ISSUE_WIDTH="${width}" \
        MITTENS_CPU_INSTRUCTION_QUANTUM="${quantum}" \
        MITTENS_CPU_PROFILE="${profile}" \
        MITTENS_CPU_TASK_TRACE="${tasks}" \
        SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
            "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${log}"

        grep -q "QEMU tile 0 exited with exit status 0" "${log}"
        require_file "${profile}/tile-0-summary.csv"
        require_file "${tasks}/tile-0.csv"
        trial_specs+=(
            "${width}:${quantum}:${profile}/tile-0-summary.csv:${tasks}/tile-0.csv"
        )
    done
done

"${ANALYZER}" "${RESULTS}" "${trial_specs[@]}"
echo "scalar/RVV issue-width and instruction-quantum timing: PASS"
