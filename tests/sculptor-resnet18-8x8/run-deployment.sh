#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly OUTPUT_DIR="${BUILD_ROOT}/tests/sculptor-resnet18-8x8/deployment"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly STATISTICS="${MITTENS_RESNET18_STATS_PATH:-${OUTPUT_DIR}/router-statistics.csv}"
readonly SIMULATION_OUTPUT="${MITTENS_RESNET18_LOG_PATH:-${OUTPUT_DIR}/simulation.log}"
readonly SST_THREADS="${MITTENS_RESNET18_SST_THREADS:-1}"
readonly TASK_TRACE="${MITTENS_RESNET18_TASK_TRACE:-0}"
readonly TASK_TRACE_OUTPUT="${MITTENS_RESNET18_TASK_TRACE_PATH:-${OUTPUT_DIR}/task-trace.csv}"
readonly TASK_TRACE_SUMMARY="${MITTENS_RESNET18_TASK_TRACE_SUMMARY_PATH:-${OUTPUT_DIR}/task-trace-summary.csv}"
readonly TASK_TRACE_GAPS="${MITTENS_RESNET18_TASK_TRACE_GAPS_PATH:-${OUTPUT_DIR}/task-trace-gaps.csv}"
readonly TASK_TRACE_RAW_DIRECTORY="${MITTENS_RESNET18_TASK_TRACE_RAW_DIRECTORY:-${OUTPUT_DIR}/task-trace-raw}"
readonly CPU_CLOCK="${MITTENS_RESNET18_CPU_CLOCK:-1GHz}"

for executable in "${QEMU}" "${SST}"; do
    require_executable "${executable}"
done
if [[ ! "${SST_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_RESNET18_SST_THREADS must be a positive integer" >&2
    exit 1
fi
case "${TASK_TRACE}" in
    0|1) ;;
    *)
        echo "MITTENS_RESNET18_TASK_TRACE must be 0 or 1" >&2
        exit 1
        ;;
esac
if [[ -z "${CPU_CLOCK}" ]]; then
    echo "MITTENS_RESNET18_CPU_CLOCK must not be empty" >&2
    exit 1
fi

"${TEST_DIR}/build-deployment.sh"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_RESNET18_IDLE_ELF="${OUTPUT_DIR}/idle.elf"
export MITTENS_RESNET18_STATS="${STATISTICS}"
if [[ "${TASK_TRACE}" == "1" ]]; then
    export MITTENS_RESNET18_TASK_TRACE_RAW_DIRECTORY="${TASK_TRACE_RAW_DIRECTORY}"
fi
for core_id in {0..18}; do
    export "MITTENS_RESNET18_CORE${core_id}_ELF=${OUTPUT_DIR}/core-${core_id}.elf"
done

echo "ResNet-18 QEMU/SST sync quantum: ${MITTENS_RESNET18_SYNC_QUANTUM:-1000000} instructions"
echo "ResNet-18 SST worker threads: ${SST_THREADS}"
echo "ResNet-18 CPU clock: ${CPU_CLOCK}"
echo "ResNet-18 mesh link: ${MITTENS_RESNET18_MESH_LINK_WIDTH_BITS:-32} bits at ${MITTENS_RESNET18_MESH_LINK_CLOCK:-1GHz}"
echo "ResNet-18 RX DMA: ${MITTENS_RESNET18_RX_DMA_WIDTH_BITS:-256} bits at ${MITTENS_RESNET18_RX_DMA_CLOCK:-1GHz}, setup=${MITTENS_RESNET18_RX_DMA_SETUP_CYCLES:-8} cycles, queue=${MITTENS_RESNET18_RX_DMA_QUEUE_DEPTH:-4}"
echo "ResNet-18 task tracing: ${TASK_TRACE}"
rm -f -- \
    "${STATISTICS}" \
    "${SIMULATION_OUTPUT}" \
    "${TASK_TRACE_OUTPUT}" \
    "${TASK_TRACE_SUMMARY}" \
    "${TASK_TRACE_GAPS}"
mkdir -p -- "${TASK_TRACE_RAW_DIRECTORY}"
find "${TASK_TRACE_RAW_DIRECTORY}" \
    -maxdepth 1 \
    -type f \
    -name 'tile-*.csv' \
    -delete
"${SST}" --num-threads="${SST_THREADS}" \
    "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"

grep -F "RESNET18_OUTPUT top1=620" "${SIMULATION_OUTPUT}" >/dev/null
for core_id in {0..18}; do
    grep -F "RESNET18_TILE_PASS core=${core_id}" \
        "${SIMULATION_OUTPUT}" >/dev/null
done

if [[ "${TASK_TRACE}" == "1" ]]; then
    python3 "${TEST_DIR}/analyze-task-trace.py" \
        "${TASK_TRACE_RAW_DIRECTORY}" \
        "${TASK_TRACE_OUTPUT}" \
        "${TASK_TRACE_SUMMARY}" \
        "${TASK_TRACE_GAPS}"
    echo "ResNet-18 raw task traces: ${TASK_TRACE_RAW_DIRECTORY}"
    echo "ResNet-18 task trace events: ${TASK_TRACE_OUTPUT}"
    echo "ResNet-18 task trace summary: ${TASK_TRACE_SUMMARY}"
    echo "ResNet-18 task trace gaps: ${TASK_TRACE_GAPS}"
fi

echo "ResNet-18 -> 19 active ELFs -> 8x8 QEMU/SST mesh: PASS"
