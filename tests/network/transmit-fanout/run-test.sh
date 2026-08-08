#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/transmit-fanout"
readonly RESULTS_DIR="${OUTPUT_DIR}/results"
readonly MODE="${MITTENS_FANOUT_MODE:-polling}"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly FANOUTS="${MITTENS_FANOUTS:-1 2 4 8 16 24}"
readonly QUANTA="${MITTENS_FANOUT_QUANTA:-1000000 100000 10000}"

case "${MODE}" in
    polling|blocking)
        readonly ELF_PREFIX="fanout"
        readonly INDEPENDENT_TASKS=0
        ;;
    overlap-blocking)
        readonly ELF_PREFIX="overlap-blocking"
        readonly INDEPENDENT_TASKS=1
        ;;
    async)
        readonly ELF_PREFIX="overlap-async"
        readonly INDEPENDENT_TASKS=1
        ;;
    *)
        echo "unsupported MITTENS_FANOUT_MODE: ${MODE}" >&2
        exit 1
        ;;
esac

"${TEST_DIR}/build-test.sh"
export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"

for fanout in ${FANOUTS}; do
    for quantum in ${QUANTA}; do
        trial="${RESULTS_DIR}/${MODE}/fanout-${fanout}/quantum-${quantum}"
        raw="${trial}/raw"
        report="${trial}/report"
        manifest="${trial}/routes.csv"
        stats="${trial}/router-statistics.csv"
        log="${trial}/simulation.log"
        mkdir -p -- "${raw}" "${report}"
        find "${raw}" -maxdepth 1 -type f -name 'tile-*.csv' -delete
        find "${report}" -maxdepth 1 -type f \
            \( -name '*.csv' -o -name '*.json' \) -delete

        {
            echo "route_id,source_core,source_task,source_output,destination_core,destination_task,destination_input,resource_id,byte_size,payload_words,manhattan_hops"
            for ((index = 0; index < fanout; ++index)); do
                source_task=11
                if (( INDEPENDENT_TASKS != 0 )); then
                    source_task=$((11 + index))
                fi
                echo "$((1000 + index)),0,${source_task},0,1,$((100 + index)),0,$((10000 + index)),2048,512,1"
            done
        } >"${manifest}"

        export MITTENS_FANOUT_TILE0_ELF="${OUTPUT_DIR}/${ELF_PREFIX}-${fanout}-tile0.elf"
        export MITTENS_FANOUT_TILE1_ELF="${OUTPUT_DIR}/${ELF_PREFIX}-${fanout}-tile1.elf"
        export MITTENS_FANOUT_STATS="${stats}"
        export MITTENS_FANOUT_SYNC_QUANTUM="${quantum}"
        export MITTENS_FANOUT_PROFILE_RAW="${raw}"

        echo "fan-out trial: mode=${MODE} fanout=${fanout} quantum=${quantum}"
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${log}"
        grep -F "FANOUT_SOURCE_PASS fanout=${fanout}" "${log}" >/dev/null
        grep -F "FANOUT_DESTINATION_PASS fanout=${fanout}" "${log}" >/dev/null
        python3 "${ANALYZER}" \
            "${raw}" \
            "${report}" \
            --router-statistics "${stats}" \
            --task-trace-directory "${raw}" \
            --route-manifest "${manifest}" \
            --mesh-link-width-bits 32 \
            --mesh-link-clock 1GHz
    done
done

python3 "${TEST_DIR}/analyze-results.py" \
    "${RESULTS_DIR}" \
    --mode "${MODE}"
echo "transmit fan-out ${MODE} matrix: PASS"
