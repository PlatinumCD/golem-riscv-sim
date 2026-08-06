#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly OUTPUT_DIR="${MITTENS_RESNET18_OUTPUT_DIR:-${BUILD_ROOT}/tests/sculptor-resnet18-8x8/deployment}"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly STATISTICS="${MITTENS_RESNET18_STATS_PATH:-${OUTPUT_DIR}/router-statistics.csv}"
readonly SIMULATION_OUTPUT="${MITTENS_RESNET18_LOG_PATH:-${OUTPUT_DIR}/simulation.log}"
readonly SST_THREADS="${MITTENS_RESNET18_SST_THREADS:-1}"
readonly VISUALIZATION_EXPORT="${MITTENS_VISUALIZATION_EXPORT:-0}"
profile_mode="${MITTENS_RESNET18_PROFILE_MODE:-off}"
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    profile_mode=trace
fi
readonly PROFILE_MODE="${profile_mode}"
unset profile_mode
TASK_TRACE="${MITTENS_RESNET18_TASK_TRACE:-0}"
if [[ "${PROFILE_MODE}" == "trace" ]]; then
    TASK_TRACE=1
fi
readonly TASK_TRACE
readonly TASK_TRACE_OUTPUT="${MITTENS_RESNET18_TASK_TRACE_PATH:-${OUTPUT_DIR}/task-trace.csv}"
readonly TASK_TRACE_SUMMARY="${MITTENS_RESNET18_TASK_TRACE_SUMMARY_PATH:-${OUTPUT_DIR}/task-trace-summary.csv}"
readonly TASK_TRACE_GAPS="${MITTENS_RESNET18_TASK_TRACE_GAPS_PATH:-${OUTPUT_DIR}/task-trace-gaps.csv}"
readonly TASK_TRACE_RAW_DIRECTORY="${MITTENS_RESNET18_TASK_TRACE_RAW_DIRECTORY:-${OUTPUT_DIR}/task-trace-raw}"
readonly CPU_CLOCK="${MITTENS_RESNET18_CPU_CLOCK:-1GHz}"
readonly CPU_ISSUE_WIDTH="${MITTENS_RESNET18_CPU_ISSUE_WIDTH:-1}"
readonly ACTIVE_CORE_MANIFEST="${OUTPUT_DIR}/active-cores.txt"
readonly PROFILE_RAW_DIRECTORY="${MITTENS_RESNET18_PROFILE_RAW_DIRECTORY:-${OUTPUT_DIR}/performance-profile-raw}"
readonly PROFILE_OUTPUT_DIRECTORY="${MITTENS_RESNET18_PROFILE_OUTPUT_DIRECTORY:-${OUTPUT_DIR}/performance-profile}"
readonly ROUTE_MANIFEST="${OUTPUT_DIR}/deployment-routes.csv"
readonly PROFILE_ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly VISUALIZATION_EXPORTER="${PROJECT_ROOT}/visualizer/exporter/export-profile.py"
readonly VISUALIZATION_DIRECTORY="${MITTENS_VISUALIZATION_DIRECTORY:-${OUTPUT_DIR}/visualization}"
readonly VISUALIZATION_TRACE="${VISUALIZATION_DIRECTORY}/trace.json"

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
case "${PROFILE_MODE}" in
    off|summary|trace) ;;
    *)
        echo "MITTENS_RESNET18_PROFILE_MODE must be off, summary, or trace" >&2
        exit 1
        ;;
esac
case "${VISUALIZATION_EXPORT}" in
    0|1) ;;
    *)
        echo "MITTENS_VISUALIZATION_EXPORT must be 0 or 1" >&2
        exit 1
        ;;
esac
if [[ "${PROFILE_MODE}" != "off" ]]; then
    require_executable "${PROFILE_ANALYZER}"
fi
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    require_executable "${VISUALIZATION_EXPORTER}"
fi
if [[ -z "${CPU_CLOCK}" ]]; then
    echo "MITTENS_RESNET18_CPU_CLOCK must not be empty" >&2
    exit 1
fi
case "${CPU_ISSUE_WIDTH}" in
    1|2|4) ;;
    *)
        echo "MITTENS_RESNET18_CPU_ISSUE_WIDTH must be 1, 2, or 4" >&2
        exit 1
        ;;
esac

export MITTENS_RESNET18_TASK_TRACE="${TASK_TRACE}"
export MITTENS_RESNET18_PROFILE_MODE="${PROFILE_MODE}"
"${TEST_DIR}/build-deployment.sh"
require_file "${ACTIVE_CORE_MANIFEST}"
mapfile -t ACTIVE_CORE_IDS <"${ACTIVE_CORE_MANIFEST}"
if [[ "${#ACTIVE_CORE_IDS[@]}" -eq 0 ]]; then
    echo "ResNet-18 active-core manifest is empty" >&2
    exit 1
fi

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_RESNET18_IDLE_ELF="${OUTPUT_DIR}/idle.elf"
export MITTENS_RESNET18_STATS="${STATISTICS}"
export MITTENS_RESNET18_ACTIVE_CORE_IDS="$(
    IFS=,
    echo "${ACTIVE_CORE_IDS[*]}"
)"
if [[ "${TASK_TRACE}" == "1" ]]; then
    export MITTENS_RESNET18_TASK_TRACE_RAW_DIRECTORY="${TASK_TRACE_RAW_DIRECTORY}"
fi
if [[ "${PROFILE_MODE}" != "off" ]]; then
    export MITTENS_RESNET18_PROFILE_RAW_DIRECTORY="${PROFILE_RAW_DIRECTORY}"
fi
for core_id in "${ACTIVE_CORE_IDS[@]}"; do
    if [[ ! "${core_id}" =~ ^[0-9]+$ ]] || ((core_id >= 64)); then
        echo "invalid ResNet-18 active core ID in manifest: ${core_id}" >&2
        exit 1
    fi
    export "MITTENS_RESNET18_CORE${core_id}_ELF=${OUTPUT_DIR}/core-${core_id}.elf"
done

echo "ResNet-18 active cores: ${ACTIVE_CORE_IDS[*]}"
echo "ResNet-18 QEMU/SST sync quantum: ${MITTENS_RESNET18_SYNC_QUANTUM:-1000000} instructions"
echo "ResNet-18 SST worker threads: ${SST_THREADS}"
echo "ResNet-18 CPU clock: ${CPU_CLOCK}"
echo "ResNet-18 CPU scalar issue width: ${CPU_ISSUE_WIDTH}"
echo "ResNet-18 analog MVM compute latency: ${MITTENS_RESNET18_ANALOG_COMPUTE_LATENCY_CYCLES:-100} cycles"
echo "ResNet-18 mesh link: ${MITTENS_RESNET18_MESH_LINK_WIDTH_BITS:-32} bits at ${MITTENS_RESNET18_MESH_LINK_CLOCK:-1GHz}"
echo "ResNet-18 RX DMA: ${MITTENS_RESNET18_RX_DMA_WIDTH_BITS:-256} bits at ${MITTENS_RESNET18_RX_DMA_CLOCK:-1GHz}, setup=${MITTENS_RESNET18_RX_DMA_SETUP_CYCLES:-8} cycles, queue=${MITTENS_RESNET18_RX_DMA_QUEUE_DEPTH:-4}"
echo "ResNet-18 task tracing: ${TASK_TRACE}"
echo "ResNet-18 performance profiling: ${PROFILE_MODE}"
echo "ResNet-18 visualization export: ${VISUALIZATION_EXPORT}"
rm -f -- \
    "${STATISTICS}" \
    "${SIMULATION_OUTPUT}" \
    "${TASK_TRACE_OUTPUT}" \
    "${TASK_TRACE_SUMMARY}" \
    "${TASK_TRACE_GAPS}"
if [[ "${PROFILE_MODE}" != "off" ]]; then
    mkdir -p -- "${PROFILE_RAW_DIRECTORY}" "${PROFILE_OUTPUT_DIRECTORY}"
    find "${PROFILE_RAW_DIRECTORY}" \
        -maxdepth 1 \
        -type f \
        -name 'tile-*.csv' \
        -delete
    find "${PROFILE_OUTPUT_DIRECTORY}" \
        -maxdepth 1 \
        -type f \
        \( -name '*.csv' -o -name '*.json' \) \
        -delete
fi
mkdir -p -- "${TASK_TRACE_RAW_DIRECTORY}"
find "${TASK_TRACE_RAW_DIRECTORY}" \
    -maxdepth 1 \
    -type f \
    -name 'tile-*.csv' \
    -delete
"${SST}" --num-threads="${SST_THREADS}" \
    "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"

grep -F "RESNET18_OUTPUT top1=620" "${SIMULATION_OUTPUT}" >/dev/null
for core_id in "${ACTIVE_CORE_IDS[@]}"; do
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

if [[ "${PROFILE_MODE}" != "off" ]]; then
    analyzer_arguments=(
        "${PROFILE_RAW_DIRECTORY}"
        "${PROFILE_OUTPUT_DIRECTORY}"
        "--router-statistics"
        "${STATISTICS}"
        "--mesh-link-width-bits"
        "${MITTENS_RESNET18_MESH_LINK_WIDTH_BITS:-32}"
        "--mesh-link-clock"
        "${MITTENS_RESNET18_MESH_LINK_CLOCK:-1GHz}"
    )
    if [[ -f "${ROUTE_MANIFEST}" ]]; then
        analyzer_arguments+=("--route-manifest" "${ROUTE_MANIFEST}")
    fi
    if [[ "${TASK_TRACE}" == "1" ]]; then
        analyzer_arguments+=(
            "--task-trace-directory"
            "${TASK_TRACE_RAW_DIRECTORY}"
        )
    fi
    python3 "${PROFILE_ANALYZER}" "${analyzer_arguments[@]}"
    echo "ResNet-18 raw performance profile: ${PROFILE_RAW_DIRECTORY}"
    echo "ResNet-18 performance report: ${PROFILE_OUTPUT_DIRECTORY}"
fi
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    python3 "${VISUALIZATION_EXPORTER}" \
        "${PROFILE_OUTPUT_DIRECTORY}" \
        "${VISUALIZATION_TRACE}" \
        --width 8 \
        --height 8 \
        --title "ResNet-18 / 8x8 mesh" \
        --source "${OUTPUT_DIR}"
    relative_trace="$(
        realpath --relative-to="${PROJECT_ROOT}" "${VISUALIZATION_TRACE}"
    )"
    echo "ResNet-18 visualization trace: ${VISUALIZATION_TRACE}"
    echo "View with: ./visualizer/serve.sh 8000 '${VISUALIZATION_TRACE}'"
    echo "Viewer URL: http://127.0.0.1:8000/visualizer/web/?data=/${relative_trace}"
fi

echo "ResNet-18 -> ${#ACTIVE_CORE_IDS[@]} active ELFs -> 8x8 QEMU/SST mesh: PASS"
