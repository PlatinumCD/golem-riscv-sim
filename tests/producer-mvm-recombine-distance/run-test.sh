#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly MODE="${1:---quick}"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_ROOT="${BUILD_ROOT}/tests/producer-mvm-recombine-distance"
readonly CONFIGURATIONS="${OUTPUT_ROOT}/configurations.tsv"
readonly IDLE_ELF="${OUTPUT_ROOT}/common/idle.elf"
readonly ANALYZER="${TEST_DIR}/analyze-results.py"

case "${MODE}" in
    --quick|--full) ;;
    *)
        echo "usage: $0 [--quick|--full]" >&2
        exit 2
        ;;
esac

require_executable "${SST}"
require_executable "${QEMU}"
require_executable "${ANALYZER}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"

"${TEST_DIR}/build-test.sh"
require_file "${CONFIGURATIONS}"
require_file "${IDLE_ELF}"

if [[ "${MODE}" == "--quick" ]]; then
    readonly RUN_LABEL=quick
    readonly QUICK_PATTERN='^(activation-d0|activation-d8|partial-d8|combined-in8-out8)$'
else
    readonly RUN_LABEL=full
fi

readonly RUN_ROOT="${OUTPUT_ROOT}/runs/${RUN_LABEL}"
readonly RUN_MANIFEST="${RUN_ROOT}/trials.tsv"
readonly RESULTS="${RUN_ROOT}/results.csv"
mkdir -p -- "${RUN_ROOT}"
printf 'name\texperiment\tinput_distance\toutput_distance\tproducer_tile\tmvm_tile\trecombine_tile\trun_directory\n' \
    > "${RUN_MANIFEST}"

while IFS=$'\t' read -r \
    name experiment input_distance output_distance \
    producer mvm recombine; do
    if [[ "${name}" == "name" ]]; then
        continue
    fi
    if [[ "${MODE}" == "--quick" && ! "${name}" =~ ${QUICK_PATTERN} ]]; then
        continue
    fi

    trial="${RUN_ROOT}/${name}"
    profile="${trial}/profile"
    tasks="${trial}/tasks"
    statistics="${trial}/router-statistics.csv"
    log="${trial}/simulation.log"
    image_directory="${OUTPUT_ROOT}/configurations/${name}"
    mkdir -p -- "${profile}" "${tasks}"
    rm -f -- "${profile}"/tile-*.csv \
        "${tasks}"/tile-*.csv \
        "${statistics}" "${log}"

    printf '[PMR distance] %s input=%s output=%s tiles=%s->%s->%s\n' \
        "${name}" "${input_distance}" "${output_distance}" \
        "${producer}" "${mvm}" "${recombine}"

    MITTENS_TEST_QEMU="${QEMU}" \
    MITTENS_PMR_PRODUCER_TILE="${producer}" \
    MITTENS_PMR_MVM_TILE="${mvm}" \
    MITTENS_PMR_RECOMBINE_TILE="${recombine}" \
    MITTENS_PMR_IMAGE_DIRECTORY="${image_directory}" \
    MITTENS_PMR_IDLE_ELF="${IDLE_ELF}" \
    MITTENS_PMR_STATISTICS="${statistics}" \
    MITTENS_PMR_PROFILE_DIRECTORY="${profile}" \
    MITTENS_PMR_TASK_DIRECTORY="${tasks}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        "${SST}" "${TEST_DIR}/simulation.py" </dev/null 2>&1 | tee "${log}"

    grep -F "PMR_PASS producer=${producer} mvm=${mvm} recombine=${recombine}" \
        "${log}" >/dev/null
    if grep -F "PMR_FAIL" "${log}" >/dev/null; then
        echo "${name}: guest reported failure" >&2
        exit 1
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${name}" "${experiment}" \
        "${input_distance}" "${output_distance}" \
        "${producer}" "${mvm}" "${recombine}" "${trial}" \
        >> "${RUN_MANIFEST}"
done < "${CONFIGURATIONS}"

"${ANALYZER}" "${RUN_MANIFEST}" "${RESULTS}"
echo "producer-MVM-recombine ${RUN_LABEL} test: PASS"
