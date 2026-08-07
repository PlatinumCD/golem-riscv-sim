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
readonly JOBS="${SCULPTOR_CORE_BUILD_JOBS:-$(nproc)}"
readonly LTO="${SCULPTOR_CORE_LTO:-full}"
readonly REGALLOC="${SCULPTOR_CORE_REGALLOC:-default}"
readonly REGALLOC_FALLBACK="${SCULPTOR_CORE_REGALLOC_FALLBACK:-none}"
readonly REUSE_OBJECTS="${SCULPTOR_CORE_REUSE_OBJECTS:-0}"
readonly ACTIVE_CORE_MANIFEST="${SCULPTOR_ACTIVE_CORE_MANIFEST:-$(dirname -- "${CORES}")/active-cores.txt}"
readonly REGALLOC_FALLBACK_MANIFEST="${SCULPTOR_CORE_REGALLOC_FALLBACK_MANIFEST:-$(dirname -- "${CORES}")/regalloc-fallback-cores.txt}"
readonly SCRATCHPAD_BYTES="${SCULPTOR_CORE_SCRATCHPAD_BYTES:-0}"
readonly OPT="${SCULPTOR_OPT:-${INSTALL_ROOT}/sculptor-mlir/bin/sculptor-mlir-opt}"
readonly TRANSLATE="${INSTALL_ROOT}/llvm/bin/mlir-translate"
readonly CLANG="${INSTALL_ROOT}/llvm/bin/clang"
readonly LLVM_READOBJ="${INSTALL_ROOT}/llvm/bin/llvm-readobj"

if [[ -z "${DEPLOYMENT}" ]]; then
    echo "SCULPTOR_DEPLOYMENT_MLIR must name the outlined Sculptor tile deployment" >&2
    exit 1
fi
require_file "${DEPLOYMENT}"
for executable in "${OPT}" "${TRANSLATE}" "${CLANG}" "${LLVM_READOBJ}"; do
    require_executable "${executable}"
done
if [[ ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
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
if [[ "${REUSE_OBJECTS}" != 0 && "${REUSE_OBJECTS}" != 1 ]]; then
    echo "SCULPTOR_CORE_REUSE_OBJECTS must be 0 or 1" >&2
    exit 1
fi

optimization_flags=(-O3)
if [[ "${LTO}" != "none" ]]; then
    optimization_flags+=("-flto=${LTO}")
fi
export SCULPTOR_CORE_OPTIMIZATION_FLAGS="${optimization_flags[*]}"

mapfile -t CORE_IDS < <(
    rg -o '^  module @tile_[0-9]+' "${DEPLOYMENT}" |
        sed -E 's/^  module @tile_//' |
        sort -n -u
)
if [[ "${#CORE_IDS[@]}" -eq 0 ]]; then
    echo "deployment module does not contain active tile modules" >&2
    exit 1
fi
mkdir -p -- "${CORES}" "$(dirname -- "${ACTIVE_CORE_MANIFEST}")"

build_core() {
    local core_id="$1"
    local prefix="${CORES}/core-${core_id}"
    local fallback_marker="${prefix}.regalloc-fallback"
    local -a clang_args

    if [[ "${REUSE_OBJECTS}" == 1 &&
          -s "${prefix}.o" ]] &&
       "${LLVM_READOBJ}" --file-headers "${prefix}.o" >/dev/null 2>&1; then
        echo "[core ${core_id}] reuse existing object"
        return
    fi
    rm -f -- "${prefix}.o" "${fallback_marker}"

    echo "[tile ${core_id}] extract tile"
    "${OPT}" "${DEPLOYMENT}" \
        "--sculptor-extract-tile-module=tile-id=${core_id}" \
        -o "${prefix}-extracted.mlir"

    echo "[tile ${core_id}] materialize runtime graph"
    "${OPT}" "${prefix}-extracted.mlir" \
        --sculptor-materialize-tile-runtime-graph \
        -o "${prefix}-runtime-graph.mlir"

    finalization_input="${prefix}-runtime-graph.mlir"
    if [[ "${SCRATCHPAD_BYTES}" != 0 ]]; then
        echo "[tile ${core_id}] plan scratchpad"
        "${OPT}" "${prefix}-runtime-graph.mlir" \
            "--sculptor-plan-tile-scratchpad=bytes=${SCRATCHPAD_BYTES}" \
            -o "${prefix}-planned.mlir"
        finalization_input="${prefix}-planned.mlir"
    fi

    echo "[tile ${core_id}] finalize runtime graph"
    "${OPT}" "${finalization_input}" \
        --sculptor-finalize-tile-runtime-graph \
        -o "${prefix}-finalized.mlir"

    echo "[core ${core_id}] bufferize, deallocate, and lower"
    "${OPT}" "${prefix}-finalized.mlir" \
        --sculptor-lower-golem-to-llvm-shims \
        --canonicalize \
        --cse \
        --empty-tensor-to-alloc-tensor \
        "--one-shot-bufferize=bufferize-function-boundaries function-boundary-type-conversion=identity-layout-map" \
        '--buffer-results-to-out-params=hoist-static-allocs' \
        --convert-bufferization-to-memref \
        --buffer-hoisting \
        --buffer-loop-hoisting \
        --buffer-deallocation-pipeline \
        --optimize-allocation-liveness \
        --convert-linalg-to-loops \
        --lower-affine \
        --convert-scf-to-cf \
        --convert-vector-to-llvm \
        --convert-math-to-libm \
        --convert-math-to-llvm \
        --expand-strided-metadata \
        --lower-affine \
        --convert-arith-to-llvm \
        --convert-index-to-llvm \
        --convert-cf-to-llvm \
        --finalize-memref-to-llvm \
        --convert-func-to-llvm \
        --reconcile-unrealized-casts \
        -o "${prefix}-compute-llvm.mlir"

    echo "[core ${core_id}] emit tile ABI"
    "${OPT}" "${prefix}-compute-llvm.mlir" \
        --sculptor-emit-golem-tile-abi \
        --sculptor-finalize-golem-intrinsics \
        -o "${prefix}-task-only.mlir"

    echo "[core ${core_id}] translate and compile"
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
        -o "${prefix}.o"
    )
    if [[ "${REGALLOC}" != "default" ]]; then
        clang_args+=(-mllvm "-regalloc=${REGALLOC}")
    fi
    if ! "${CLANG}" "${clang_args[@]}"; then
        if [[ "${REGALLOC_FALLBACK}" == "none" ]]; then
            return 1
        fi
        echo "[core ${core_id}] ${REGALLOC} register allocation failed; retry with ${REGALLOC_FALLBACK}" >&2
        rm -f -- "${prefix}.o"
        "${CLANG}" \
            "${clang_args[@]}" \
            -mllvm "-regalloc=${REGALLOC_FALLBACK}"
        printf '%s\n' "${REGALLOC_FALLBACK}" >"${fallback_marker}"
    fi

    echo "[core ${core_id}] complete"
}
export -f build_core
export OPT TRANSLATE CLANG LLVM_READOBJ DEPLOYMENT CORES GOLEM_TARGET GOLEM_CPU GOLEM_ABI
export REGALLOC REGALLOC_FALLBACK SCRATCHPAD_BYTES
export REUSE_OBJECTS

printf '%s\n' "${CORE_IDS[@]}" |
    xargs -n1 "-P${JOBS}" bash -euo pipefail -c 'build_core "$1"' _

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
echo "built ${#CORE_IDS[@]} Sculptor core objects with ${JOBS} workers"
