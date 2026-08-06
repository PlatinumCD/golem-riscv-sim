#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly COMMON_SCRIPT="$(cd -- "${TEST_DIR}/../.." && pwd)/build-scripts/common.sh"
# shellcheck source=../../build-scripts/common.sh
source "${COMMON_SCRIPT}"

readonly OUTPUT_DIR="${MITTENS_GPT2_DEPLOYMENT_DIR:-${BUILD_ROOT}/tests/sculptor-gpt2-8x8/deployment}"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly STATISTICS="${OUTPUT_DIR}/router-statistics.csv"
readonly SIMULATION_OUTPUT="${OUTPUT_DIR}/simulation.log"
readonly SST_THREADS="${MITTENS_GPT2_SST_THREADS:-1}"
readonly ACTIVE_CORE_MANIFEST="${OUTPUT_DIR}/active-cores.txt"
readonly SEQUENCE_LENGTH="${MITTENS_GPT2_SEQUENCE_LENGTH:-4}"
readonly CPU_ISSUE_WIDTH="${MITTENS_GPT2_CPU_ISSUE_WIDTH:-1}"
readonly MESH_WIDTH="${MITTENS_GPT2_MESH_WIDTH:-8}"
readonly MESH_HEIGHT="${MITTENS_GPT2_MESH_HEIGHT:-8}"
readonly MEMORY_BACKEND="${MITTENS_MEMORY_BACKEND:-native}"
readonly MEMORY_TOPOLOGY="${MITTENS_MEMORY_TOPOLOGY:-private_l1}"
readonly VISUALIZATION_EXPORT="${MITTENS_VISUALIZATION_EXPORT:-0}"
profile_mode="${MITTENS_GPT2_PROFILE_MODE:-off}"
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    profile_mode=trace
fi
readonly PROFILE_MODE="${profile_mode}"
unset profile_mode
readonly TRANSMIT_POLICY="${MITTENS_GPT2_TRANSMIT_POLICY:-blocking}"
readonly PROFILE_RAW_DIRECTORY="${MITTENS_GPT2_PROFILE_RAW_DIRECTORY:-${OUTPUT_DIR}/performance-profile-raw}"
readonly PROFILE_OUTPUT_DIRECTORY="${MITTENS_GPT2_PROFILE_OUTPUT_DIRECTORY:-${OUTPUT_DIR}/performance-profile}"
readonly ROUTE_MANIFEST="${OUTPUT_DIR}/deployment-routes.csv"
readonly PROFILE_ANALYZER="${PROJECT_ROOT}/scripts/analyze-performance-profile.py"
readonly LLVM_SYMBOLIZER="${INSTALL_ROOT}/llvm/bin/llvm-symbolizer"
readonly VISUALIZATION_EXPORTER="${PROJECT_ROOT}/visualizer/exporter/export-profile.py"
readonly VISUALIZATION_DIRECTORY="${MITTENS_VISUALIZATION_DIRECTORY:-${OUTPUT_DIR}/visualization}"
readonly VISUALIZATION_TRACE="${VISUALIZATION_DIRECTORY}/trace.json"

for executable in "${QEMU}" "${SST}"; do
    require_executable "${executable}"
done
if [[ ! "${SST_THREADS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_SST_THREADS must be a positive integer" >&2
    exit 1
fi
if [[ ! "${SEQUENCE_LENGTH}" =~ ^[1-9][0-9]*$ ]]; then
    echo "MITTENS_GPT2_SEQUENCE_LENGTH must be a positive integer" >&2
    exit 1
fi
if [[ ! "${MESH_WIDTH}" =~ ^[1-9][0-9]*$ ||
      ! "${MESH_HEIGHT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "GPT-2 mesh dimensions must be positive integers" >&2
    exit 1
fi
readonly NETWORK_SIZE=$((MESH_WIDTH * MESH_HEIGHT))
case "${CPU_ISSUE_WIDTH}" in
    1|2|4) ;;
    *)
        echo "MITTENS_GPT2_CPU_ISSUE_WIDTH must be 1, 2, or 4" >&2
        exit 1
        ;;
esac
if [[ "${MEMORY_BACKEND}" != "native" &&
      "${MEMORY_BACKEND}" != "memhierarchy" ]]; then
    echo "MITTENS_MEMORY_BACKEND must be native or memhierarchy" >&2
    exit 1
fi
if [[ "${MEMORY_TOPOLOGY}" != "private_l1" &&
      "${MEMORY_TOPOLOGY}" != "shared_l2" ]]; then
    echo "MITTENS_MEMORY_TOPOLOGY must be private_l1 or shared_l2" >&2
    exit 1
fi
if [[ "${MEMORY_BACKEND}" == "native" &&
      "${MEMORY_TOPOLOGY}" != "private_l1" ]]; then
    echo "shared_l2 requires MITTENS_MEMORY_BACKEND=memhierarchy" >&2
    exit 1
fi
case "${PROFILE_MODE}" in
    off|summary|trace) ;;
    *)
        echo "MITTENS_GPT2_PROFILE_MODE must be off, summary, or trace" >&2
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
case "${TRANSMIT_POLICY}" in
    blocking|async) ;;
    *)
        echo "MITTENS_GPT2_TRANSMIT_POLICY must be blocking or async" >&2
        exit 1
        ;;
esac
if [[ "${PROFILE_MODE}" != "off" ]]; then
    require_executable "${PROFILE_ANALYZER}"
    require_executable "${LLVM_SYMBOLIZER}"
fi
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    require_executable "${VISUALIZATION_EXPORTER}"
fi
export MITTENS_GPT2_MESH_WIDTH="${MESH_WIDTH}"
export MITTENS_GPT2_MESH_HEIGHT="${MESH_HEIGHT}"
MITTENS_GPT2_PROFILE_MODE="${PROFILE_MODE}" \
    "${TEST_DIR}/build-deployment.sh"
require_file "${ACTIVE_CORE_MANIFEST}"
mapfile -t ACTIVE_CORE_IDS <"${ACTIVE_CORE_MANIFEST}"

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_GPT2_IDLE_ELF="${OUTPUT_DIR}/idle.elf"
export MITTENS_GPT2_STATS="${STATISTICS}"
export MITTENS_MEMORY_BACKEND="${MEMORY_BACKEND}"
export MITTENS_GPT2_PROFILE_MODE="${PROFILE_MODE}"
if [[ "${PROFILE_MODE}" != "off" ]]; then
    export MITTENS_GPT2_PROFILE_RAW_DIRECTORY="${PROFILE_RAW_DIRECTORY}"
fi
export MITTENS_GPT2_ACTIVE_CORE_IDS="$(
    IFS=,
    echo "${ACTIVE_CORE_IDS[*]}"
)"
for core_id in "${ACTIVE_CORE_IDS[@]}"; do
    export "MITTENS_GPT2_CORE${core_id}_ELF=${OUTPUT_DIR}/core-${core_id}.elf"
done

echo "GPT-2 active cores: ${ACTIVE_CORE_IDS[*]}"
echo "GPT-2 QEMU/SST sync quantum: ${MITTENS_GPT2_SYNC_QUANTUM:-1000000} instructions"
echo "GPT-2 SST worker threads: ${SST_THREADS}"
echo "GPT-2 mesh: ${MESH_WIDTH}x${MESH_HEIGHT} (${NETWORK_SIZE} tiles)"
echo "GPT-2 CPU clock: ${MITTENS_GPT2_CPU_CLOCK:-1GHz}"
echo "GPT-2 CPU scalar issue width: ${CPU_ISSUE_WIDTH}"
echo "GPT-2 analog MVM compute latency: ${MITTENS_GPT2_ANALOG_COMPUTE_LATENCY_CYCLES:-100} cycles"
echo "GPT-2 memory backend: ${MEMORY_BACKEND}"
echo "GPT-2 memory topology: ${MEMORY_TOPOLOGY}"
echo "GPT-2 performance profiling: ${PROFILE_MODE}"
echo "GPT-2 visualization export: ${VISUALIZATION_EXPORT}"
echo "GPT-2 transmit policy: ${TRANSMIT_POLICY}"
rm -f -- "${STATISTICS}" "${SIMULATION_OUTPUT}"
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
"${SST}" --num-threads="${SST_THREADS}" \
    "${TEST_DIR}/simulation.py" 2>&1 | tee "${SIMULATION_OUTPUT}"

# QEMU UART output from multiple tiles shares the SST process stdout and can
# interleave at character boundaries.  It is therefore diagnostic output, not
# a reliable machine-readable completion channel.  The tile program returns a
# nonzero status for every runtime or output-validation failure, and Tile turns
# every nonzero QEMU exit into an SST fatal error.  With pipefail enabled above,
# a successful SST command is the authoritative all-tile pass result.

if [[ "${MEMORY_BACKEND}" == "memhierarchy" ]]; then
    cache_summary="$(awk -F, \
        -v active="${MITTENS_GPT2_ACTIVE_CORE_IDS}" '
        BEGIN {
            active_count = split(active, active_cores, ",")
        }
        $1 ~ /^tile[0-9]+\.private_l1$/ &&
        $2 == "CacheHits" {
            hits[$1] += $7
            total_hits += $7
        }
        $1 ~ /^tile[0-9]+\.private_l1$/ &&
        $2 == "CacheMisses" {
            misses[$1] += $7
            total_misses += $7
        }
        END {
            for (core_index = 1; core_index <= active_count; ++core_index) {
                component = "tile" active_cores[core_index] ".private_l1"
                if (hits[component] + misses[component] == 0) {
                    exit 1
                }
            }
            printf "hits=%d misses=%d accesses=%d hit-rate=%.2f%%",
                   total_hits,
                   total_misses,
                   total_hits + total_misses,
                   100.0 * total_hits / (total_hits + total_misses)
        }
    ' "${STATISTICS}")" || {
        echo "GPT-2 private L1s observed no accesses" >&2
        exit 1
    }
    echo "GPT-2 private L1 total: ${cache_summary}"
fi

if [[ "${PROFILE_MODE}" != "off" ]]; then
    analyzer_arguments=(
        "${PROFILE_RAW_DIRECTORY}"
        "${PROFILE_OUTPUT_DIRECTORY}"
        "--router-statistics"
        "${STATISTICS}"
        "--mesh-link-width-bits"
        "${MITTENS_GPT2_MESH_LINK_WIDTH_BITS:-32}"
        "--mesh-link-clock"
        "${MITTENS_GPT2_MESH_LINK_CLOCK:-1GHz}"
        "--elf-directory"
        "${OUTPUT_DIR}"
        "--symbolizer"
        "${LLVM_SYMBOLIZER}"
    )
    if [[ -f "${ROUTE_MANIFEST}" ]]; then
        analyzer_arguments+=("--route-manifest" "${ROUTE_MANIFEST}")
    fi
    if [[ "${PROFILE_MODE}" == "trace" ]]; then
        analyzer_arguments+=(
            "--task-trace-directory"
            "${PROFILE_RAW_DIRECTORY}"
        )
    fi
    python3 "${PROFILE_ANALYZER}" "${analyzer_arguments[@]}"
    echo "GPT-2 raw performance profile: ${PROFILE_RAW_DIRECTORY}"
    echo "GPT-2 performance report: ${PROFILE_OUTPUT_DIRECTORY}"
fi
if [[ "${VISUALIZATION_EXPORT}" == "1" ]]; then
    python3 "${VISUALIZATION_EXPORTER}" \
        "${PROFILE_OUTPUT_DIRECTORY}" \
        "${VISUALIZATION_TRACE}" \
        --width "${MESH_WIDTH}" \
        --height "${MESH_HEIGHT}" \
        --title "GPT-2 / ${MESH_WIDTH}x${MESH_HEIGHT} mesh" \
        --source "${OUTPUT_DIR}"
    relative_trace="$(
        realpath --relative-to="${PROJECT_ROOT}" "${VISUALIZATION_TRACE}"
    )"
    echo "GPT-2 visualization trace: ${VISUALIZATION_TRACE}"
    echo "View with: ./visualizer/serve.sh 8000 '${VISUALIZATION_TRACE}'"
    echo "Viewer URL: http://127.0.0.1:8000/visualizer/web/?data=/${relative_trace}"
fi

echo "GPT-2 fixture -> ${#ACTIVE_CORE_IDS[@]} active ELFs -> ${MESH_WIDTH}x${MESH_HEIGHT} QEMU/SST mesh: PASS"
