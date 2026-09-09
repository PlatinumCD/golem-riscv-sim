#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

# SCULPTOR_DEPLOYMENT_MLIR is the RA-tree pipeline output after tile routines
# have been outlined.  Keep the former name as a compatibility alias while
# callers migrate from the retired task-graph partitioner.
readonly DEPLOYMENT="${SCULPTOR_DEPLOYMENT_MLIR:-${SCULPTOR_PARTITIONED_MLIR:-}}"
readonly CORES="${SCULPTOR_CORE_OBJECT_DIR:?SCULPTOR_CORE_OBJECT_DIR must name the output directory}"
readonly REQUESTED_JOBS="${SCULPTOR_CORE_BUILD_JOBS:-$(nproc)}"
readonly MEMORY_PER_JOB_TEXT="${SCULPTOR_CORE_MEMORY_PER_JOB_BYTES:-4GiB}"
readonly MEMORY_RESERVE_TEXT="${SCULPTOR_CORE_MEMORY_RESERVE_BYTES:-32GiB}"
readonly LTO="${SCULPTOR_CORE_LTO:-full}"
readonly REGALLOC="${SCULPTOR_CORE_REGALLOC:-default}"
readonly REGALLOC_FALLBACK="${SCULPTOR_CORE_REGALLOC_FALLBACK:-none}"
readonly REUSE_OBJECTS="${SCULPTOR_CORE_REUSE_OBJECTS:-0}"
readonly PRE_SPLIT="${SCULPTOR_CORE_PRE_SPLIT:-0}"
readonly COMPILER_STAGE_TIMEOUT_SECONDS="${SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS:-300}"
readonly ACTIVE_CORE_MANIFEST="${SCULPTOR_ACTIVE_CORE_MANIFEST:-$(dirname -- "${CORES}")/active-cores.txt}"
readonly DEPLOYMENT_MANIFEST="${SCULPTOR_DEPLOYMENT_MANIFEST:-$(dirname -- "${CORES}")/deployment-manifest.json}"
readonly REGALLOC_FALLBACK_MANIFEST="${SCULPTOR_CORE_REGALLOC_FALLBACK_MANIFEST:-$(dirname -- "${CORES}")/regalloc-fallback-cores.txt}"
readonly MEMORY_REPORT_DIR="${SCULPTOR_CORE_MEMORY_REPORT_DIR:-$(dirname -- "${CORES}")/memory-reports}"
readonly MEMORY_REPORTER="${PROJECT_ROOT}/tools/compiler/collect-sculptor-memory-reports.py"
readonly SCRATCHPAD_BYTES="${SCULPTOR_CORE_SCRATCHPAD_BYTES:-0}"
readonly COPY_VECTOR_BITS="${SCULPTOR_CORE_COPY_VECTOR_BITS:-0}"
readonly CONV_PATCH_VECTOR_BITS="${SCULPTOR_CORE_CONV_PATCH_VECTOR_BITS:-0}"
readonly DIGITAL_KERNEL_VECTOR_BITS="${SCULPTOR_CORE_DIGITAL_KERNEL_VECTOR_BITS:-0}"
readonly STRICT_MEMORY_AUDIT="${SCULPTOR_CORE_STRICT_MEMORY_AUDIT:-0}"
readonly INSTRUMENT_HEAP="${SCULPTOR_CORE_INSTRUMENT_HEAP:-0}"
readonly OPT="${SCULPTOR_OPT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt}"
readonly SPLIT="${SCULPTOR_SPLIT_TILE_DEPLOYMENT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-split-tile-deployment}"
readonly TRANSLATE="${INSTALL_ROOT}/llvm/bin/mlir-translate"
readonly CLANG="${INSTALL_ROOT}/llvm/bin/clang"
readonly LLVM_READOBJ="${INSTALL_ROOT}/llvm/bin/llvm-readobj"
readonly LLVM_BCANALYZER="${INSTALL_ROOT}/llvm/bin/llvm-bcanalyzer"

if [[ "${PRE_SPLIT}" == 0 && -z "${DEPLOYMENT}" ]]; then
    echo "SCULPTOR_DEPLOYMENT_MLIR must name the outlined Sculptor tile deployment" >&2
    exit 1
fi
require_file "${MEMORY_REPORTER}"
if [[ "${PRE_SPLIT}" == 0 ]]; then
    require_file "${DEPLOYMENT}"
fi
required_executables=(
    "${OPT}" "${TRANSLATE}" "${CLANG}" "${LLVM_READOBJ}"
    "${LLVM_BCANALYZER}"
    "${COMPILER_PYTHON}"
)
if [[ "${PRE_SPLIT}" == 0 ]]; then
    required_executables+=("${SPLIT}")
fi
for executable in "${required_executables[@]}"; do
    require_executable "${executable}"
done
require_command timeout
require_command cut
if [[ ! "${REQUESTED_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_CORE_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi
if [[ "${LTO}" != "none" && "${LTO}" != "full" && "${LTO}" != "thin" ]]; then
    echo "SCULPTOR_CORE_LTO must be none, full, or thin" >&2
    exit 1
fi
if [[ "${REGALLOC}" != "default" &&
      "${REGALLOC}" != "basic" &&
      "${REGALLOC}" != "fast" ]]; then
    echo "SCULPTOR_CORE_REGALLOC must be default, basic, or fast" >&2
    exit 1
fi
if [[ "${REGALLOC_FALLBACK}" != "none" &&
      "${REGALLOC_FALLBACK}" != "basic" &&
      "${REGALLOC_FALLBACK}" != "fast" ]]; then
    echo "SCULPTOR_CORE_REGALLOC_FALLBACK must be none, basic, or fast" >&2
    exit 1
fi
if [[ "${REGALLOC}" != "default" &&
      "${REGALLOC_FALLBACK}" != "none" ]]; then
    echo "an explicit SCULPTOR_CORE_REGALLOC cannot be combined with a fallback" >&2
    exit 1
fi
if [[ "${REUSE_OBJECTS}" != 0 && "${REUSE_OBJECTS}" != 1 ]] ||
   [[ "${PRE_SPLIT}" != 0 && "${PRE_SPLIT}" != 1 ]]; then
    echo "SCULPTOR_CORE_REUSE_OBJECTS and SCULPTOR_CORE_PRE_SPLIT must be 0 or 1" >&2
    exit 1
fi
if [[ ! "${COMPILER_STAGE_TIMEOUT_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SCULPTOR_COMPILER_STAGE_TIMEOUT_SECONDS must be a positive integer" >&2
    exit 1
fi
if [[ ! "${SCRATCHPAD_BYTES}" =~ ^[0-9]+$ ]]; then
    echo "SCULPTOR_CORE_SCRATCHPAD_BYTES must be a nonnegative integer" >&2
    exit 1
fi
if [[ ! "${COPY_VECTOR_BITS}" =~ ^[0-9]+$ ]] ||
   ((COPY_VECTOR_BITS != 0 && COPY_VECTOR_BITS % 8 != 0)); then
    echo "SCULPTOR_CORE_COPY_VECTOR_BITS must be zero or a positive multiple of 8" >&2
    exit 1
fi
if [[ ! "${CONV_PATCH_VECTOR_BITS}" =~ ^[0-9]+$ ]] ||
   ((CONV_PATCH_VECTOR_BITS != 0 && CONV_PATCH_VECTOR_BITS % 32 != 0)); then
    echo "SCULPTOR_CORE_CONV_PATCH_VECTOR_BITS must be zero or a positive multiple of 32" >&2
    exit 1
fi
if [[ ! "${DIGITAL_KERNEL_VECTOR_BITS}" =~ ^[0-9]+$ ]] ||
   ((DIGITAL_KERNEL_VECTOR_BITS != 0 &&
      DIGITAL_KERNEL_VECTOR_BITS % 32 != 0)); then
    echo "SCULPTOR_CORE_DIGITAL_KERNEL_VECTOR_BITS must be zero or a positive multiple of 32" >&2
    exit 1
fi
for value in "${STRICT_MEMORY_AUDIT}" "${INSTRUMENT_HEAP}"; do
    if [[ "${value}" != 0 && "${value}" != 1 ]]; then
        echo "strict memory audit and heap instrumentation must be 0 or 1" >&2
        exit 1
    fi
done

memory_per_job_bytes="$(parse_byte_size "${MEMORY_PER_JOB_TEXT}")"
memory_reserve_bytes="$(parse_byte_size "${MEMORY_RESERVE_TEXT}")"
if ((memory_per_job_bytes == 0)); then
    echo "SCULPTOR_CORE_MEMORY_PER_JOB_BYTES must be positive" >&2
    exit 1
fi
mem_available_kib="$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo)"
if [[ ! "${mem_available_kib}" =~ ^[0-9]+$ ]]; then
    echo "could not determine MemAvailable from /proc/meminfo" >&2
    exit 1
fi
mem_available_bytes=$((mem_available_kib * 1024))
if ((mem_available_bytes <= memory_reserve_bytes)); then
    echo "available memory does not exceed the required compiler reserve" >&2
    exit 1
fi
memory_limited_jobs=$((
    (mem_available_bytes - memory_reserve_bytes) / memory_per_job_bytes
))
if ((memory_limited_jobs < 1)); then
    echo "one compiler worker cannot fit without consuming the memory reserve" >&2
    exit 1
fi
jobs="${REQUESTED_JOBS}"
if ((jobs > memory_limited_jobs)); then
    jobs="${memory_limited_jobs}"
fi
readonly JOBS="${jobs}"
readonly CORE_VMEM_LIMIT_KIB=$(((memory_per_job_bytes + 1023) / 1024))
printf '[resource budget] requested_workers=%s effective_workers=%s available_bytes=%s reserve_bytes=%s per_worker_vmem_bytes=%s\n' \
    "${REQUESTED_JOBS}" "${JOBS}" "${mem_available_bytes}" \
    "${memory_reserve_bytes}" "${memory_per_job_bytes}"

optimization_flags=(-O3)
if [[ "${LTO}" != "none" ]]; then
    optimization_flags+=("-flto=${LTO}")
fi
export SCULPTOR_CORE_OPTIMIZATION_FLAGS="${optimization_flags[*]}"

# MLIR source locations can point at a single multi-megabyte operation line.
# Preserve the complete error message while bounding source-context lines so
# one failed parallel tile cannot expand the campaign harness log by gigabytes.
run_opt_bounded_diagnostics() {
    local command_status
    set +e
    "$@" 2>&1 | cut -c 1-8192 >&2
    command_status="${PIPESTATUS[0]}"
    set -e
    return "${command_status}"
}

mkdir -p -- "${CORES}" "$(dirname -- "${ACTIVE_CORE_MANIFEST}")"
if [[ "${REUSE_OBJECTS}" == 0 ]]; then
    # A fresh deployment may use fewer tiles than a previous deployment in the
    # same run directory.  Remove every old per-core artifact before splitting
    # so downstream discovery cannot mistake inactive cores for current ones.
    find "${CORES}" -maxdepth 1 -type f -name 'core-*' \
        ! -name 'core-*-extracted.mlir' -delete
fi
split_manifest=""
if [[ "${PRE_SPLIT}" == 1 ]]; then
    require_file "${ACTIVE_CORE_MANIFEST}"
    mapfile -t CORE_IDS < <(sort -n -u -- "${ACTIVE_CORE_MANIFEST}")
    for core_id in "${CORE_IDS[@]}"; do
        require_file "${CORES}/core-${core_id}-extracted.mlir"
    done
else
    split_manifest="$(mktemp "${ACTIVE_CORE_MANIFEST}.split.XXXXXX")"
    trap 'rm -f -- "${split_manifest}"' EXIT
    echo "[deployment] split active tiles from one parse"
    timeout --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${SPLIT}" "${DEPLOYMENT}" \
        --output-directory="${CORES}" \
        --manifest="${split_manifest}" \
        --deployment-manifest="${DEPLOYMENT_MANIFEST}"
    mapfile -t CORE_IDS < <(sort -n -u -- "${split_manifest}")
fi
if [[ "${#CORE_IDS[@]}" -eq 0 ]]; then
    echo "deployment module does not contain active tile modules" >&2
    exit 1
fi

build_core() {
    local core_id="$1"
    local prefix="${CORES}/core-${core_id}"
    local fallback_marker="${prefix}.regalloc-fallback"
    local backend_options="sculptor-audit-tile-bufferization{strict=${STRICT_MEMORY_AUDIT}}"
    local -a clang_args

    if [[ "${COPY_VECTOR_BITS}" != 0 ||
          "${CONV_PATCH_VECTOR_BITS}" != 0 ]]; then
        backend_options+=";sculptor-vectorize-tile-copies{vector-bits=${COPY_VECTOR_BITS} conv-patch-vector-bits=${CONV_PATCH_VECTOR_BITS}}"
    fi
    if [[ "${DIGITAL_KERNEL_VECTOR_BITS}" != 0 ]]; then
        backend_options+=";sculptor-vectorize-digital-kernels{vector-bits=${DIGITAL_KERNEL_VECTOR_BITS}}"
    fi
    if [[ "${INSTRUMENT_HEAP}" == 1 ]]; then
        backend_options+=";sculptor-instrument-tile-heap{}"
    fi

    if [[ "${REUSE_OBJECTS}" == 1 &&
          -s "${prefix}.o" &&
          -s "${prefix}-compute-llvm.mlir" ]] &&
       object_is_valid "${prefix}.o"; then
        echo "[core ${core_id}] reuse existing object"
        return
    fi
    rm -f -- "${prefix}.o" "${prefix}.o.tmp" "${fallback_marker}"

    echo "[tile ${core_id}] materialize runtime graph"
    run_opt_bounded_diagnostics \
        timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${OPT}" --mlir-disable-threading \
        --mlir-print-op-on-diagnostic=false "${prefix}-extracted.mlir" \
        "--sculptor-pipeline=profile=parent from=extracted through=tile-runtime" \
        -o "${prefix}-runtime-graph.mlir"

    finalization_input="${prefix}-runtime-graph.mlir"
    if [[ "${SCRATCHPAD_BYTES}" != 0 ]]; then
        echo "[tile ${core_id}] plan scratchpad"
        run_opt_bounded_diagnostics \
            timeout --foreground --signal=TERM --kill-after=10s \
            "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
            "${OPT}" --mlir-disable-threading \
            --mlir-print-op-on-diagnostic=false "${prefix}-runtime-graph.mlir" \
            "--sculptor-pipeline=profile=parent from=tile-runtime through=scratchpad pass-options={sculptor-plan-tile-scratchpad{bytes=${SCRATCHPAD_BYTES}}}" \
            -o "${prefix}-planned.mlir"
        finalization_input="${prefix}-planned.mlir"
    fi

    echo "[tile ${core_id}] finalize runtime graph"
    run_opt_bounded_diagnostics \
        timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${OPT}" --mlir-disable-threading \
        --mlir-print-op-on-diagnostic=false "${finalization_input}" \
        "--sculptor-pipeline=profile=parent from=scratchpad through=shard-residency" \
        -o "${prefix}-finalized.mlir"

    echo "[core ${core_id}] bufferize, deallocate, and lower"
    run_opt_bounded_diagnostics \
        timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${OPT}" --mlir-disable-threading \
        --mlir-print-op-on-diagnostic=false "${prefix}-finalized.mlir" \
        "--sculptor-pipeline=profile=parent from=shard-residency through=llvm pass-options={${backend_options}}" \
        -o "${prefix}-compute-llvm.mlir"

    echo "[core ${core_id}] emit tile ABI"
    run_opt_bounded_diagnostics \
        timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${OPT}" --mlir-disable-threading \
        --mlir-print-op-on-diagnostic=false "${prefix}-compute-llvm.mlir" \
        "--sculptor-pipeline=profile=parent from=llvm through=abi" \
        -o "${prefix}-task-only.mlir"

    echo "[core ${core_id}] translate and compile"
    timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${TRANSLATE}" --mlir-to-llvmir \
        "${prefix}-task-only.mlir" \
        -o "${prefix}.ll"
    read -r -a optimization_flags <<<"${SCULPTOR_CORE_OPTIMIZATION_FLAGS}"
    clang_args=(
        "--target=${GOLEM_TARGET}" \
        "-mcpu=${GOLEM_CPU}" \
        "-mabi=${GOLEM_ABI}" \
        -mcmodel=medany \
        -ffreestanding \
        -fno-stack-protector \
        -ffunction-sections \
        -fdata-sections \
        "${optimization_flags[@]}" \
        -Wno-override-module \
        -c "${prefix}.ll" \
        -o "${prefix}.o.tmp"
    )
    if [[ "${REGALLOC}" != "default" ]]; then
        clang_args+=(-mllvm "-regalloc=${REGALLOC}")
    fi
    if ! timeout --foreground --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        "${CLANG}" "${clang_args[@]}"; then
        if [[ "${REGALLOC_FALLBACK}" == "none" ]]; then
            return 1
        fi
        echo "[core ${core_id}] ${REGALLOC} register allocation failed; retry with ${REGALLOC_FALLBACK}" >&2
        rm -f -- "${prefix}.o.tmp"
        timeout --foreground --signal=TERM --kill-after=10s \
            "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
            "${CLANG}" \
            "${clang_args[@]}" \
            -mllvm "-regalloc=${REGALLOC_FALLBACK}"
        printf '%s\n' "${REGALLOC_FALLBACK}" >"${fallback_marker}"
    fi

    if ! object_is_valid "${prefix}.o.tmp"; then
        echo "[core ${core_id}] compiler produced an invalid object" >&2
        rm -f -- "${prefix}.o.tmp"
        return 1
    fi
    mv -- "${prefix}.o.tmp" "${prefix}.o"

    echo "[core ${core_id}] complete"
}

object_is_valid() {
    local object="$1"
    [[ -s "${object}" ]] || return 1
    if [[ "${LTO}" == "none" ]]; then
        "${LLVM_READOBJ}" --file-headers "${object}" >/dev/null 2>&1
    else
        "${LLVM_BCANALYZER}" "${object}" >/dev/null 2>&1
    fi
}

build_core_limited() {
    ulimit -v "${CORE_VMEM_LIMIT_KIB}"
    build_core "$1"
}
export -f build_core
export -f object_is_valid build_core_limited run_opt_bounded_diagnostics
export OPT TRANSLATE CLANG LLVM_READOBJ LLVM_BCANALYZER
export CORES GOLEM_TARGET GOLEM_CPU GOLEM_ABI LTO
export REGALLOC REGALLOC_FALLBACK SCRATCHPAD_BYTES COPY_VECTOR_BITS
export CONV_PATCH_VECTOR_BITS DIGITAL_KERNEL_VECTOR_BITS
export STRICT_MEMORY_AUDIT INSTRUMENT_HEAP
export REUSE_OBJECTS
export COMPILER_STAGE_TIMEOUT_SECONDS
export CORE_VMEM_LIMIT_KIB

printf '%s\n' "${CORE_IDS[@]}" |
    timeout --signal=TERM --kill-after=10s \
        "${COMPILER_STAGE_TIMEOUT_SECONDS}s" \
        xargs -n1 "-P${JOBS}" bash -euo pipefail \
        -c 'build_core_limited "$1"' _

for core_id in "${CORE_IDS[@]}"; do
    require_file "${CORES}/core-${core_id}.o"
done
manifest_tmp="${ACTIVE_CORE_MANIFEST}.tmp"
printf '%s\n' "${CORE_IDS[@]}" >"${manifest_tmp}"
mv -- "${manifest_tmp}" "${ACTIVE_CORE_MANIFEST}"
fallback_manifest_tmp="${REGALLOC_FALLBACK_MANIFEST}.tmp"
: >"${fallback_manifest_tmp}"
for core_id in "${CORE_IDS[@]}"; do
    if [[ -f "${CORES}/core-${core_id}.regalloc-fallback" ]]; then
        printf '%s %s\n' \
            "${core_id}" \
            "$(<"${CORES}/core-${core_id}.regalloc-fallback")" \
            >>"${fallback_manifest_tmp}"
    fi
done
mv -- "${fallback_manifest_tmp}" "${REGALLOC_FALLBACK_MANIFEST}"
if [[ -n "${split_manifest}" ]]; then
    rm -f -- "${split_manifest}"
    trap - EXIT
fi
"${COMPILER_PYTHON}" "${MEMORY_REPORTER}" \
    --core-directory "${CORES}" \
    --output-directory "${MEMORY_REPORT_DIR}"
echo "built ${#CORE_IDS[@]} Sculptor core objects with ${JOBS} workers"
