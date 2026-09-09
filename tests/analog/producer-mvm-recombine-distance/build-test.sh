#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly LLVM="${INSTALL_ROOT}/llvm"
readonly CLANG="${LLVM}/bin/clang"
readonly CLANGXX="${LLVM}/bin/clang++"
readonly OUTPUT_ROOT="${TEST_RESULTS_ROOT}/producer-mvm-recombine-distance"
readonly COMMON_OUTPUT="${OUTPUT_ROOT}/common"
readonly CONFIGURATIONS="${OUTPUT_ROOT}/configurations.tsv"
readonly WIDTH=9
readonly DISTANCES=(0 1 2 4 8)

require_executable "${CLANG}"
require_executable "${CLANGXX}"
mkdir -p -- "${COMMON_OUTPUT}"

common_flags=(
    "--target=${GOLEM_TARGET}"
    "-mcpu=${GOLEM_CPU}"
    "-mabi=${GOLEM_ABI}"
    -mcmodel=medany
    -ffreestanding
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    -O2
    -g
)
cxx_flags=(
    "${common_flags[@]}"
    -std=c++20
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    "-I${PLATFORM_ROOT}"
)

"${CLANG}" "${common_flags[@]}" \
    -c "${PLATFORM_STARTUP_ROOT}/crt0.S" \
    -o "${COMMON_OUTPUT}/crt0.o"
for source in uart platform-exit; do
    "${CLANGXX}" "${cxx_flags[@]}" \
        -c "$(platform_source "${source}.cpp")" \
        -o "${COMMON_OUTPUT}/${source}.o"
done
"${CLANGXX}" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/idle-main.cpp" \
    -o "${COMMON_OUTPUT}/idle-main.o"
"${CLANGXX}" "${common_flags[@]}" \
    -nostdlib -nostartfiles -nodefaultlibs \
    -fuse-ld=lld \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
    "${COMMON_OUTPUT}/crt0.o" \
    "${COMMON_OUTPUT}/uart.o" \
    "${COMMON_OUTPUT}/platform-exit.o" \
    "${COMMON_OUTPUT}/idle-main.o" \
    -o "${COMMON_OUTPUT}/idle.elf"

printf 'name\texperiment\tinput_distance\toutput_distance\tproducer_tile\tmvm_tile\trecombine_tile\n' \
    > "${CONFIGURATIONS}"

build_trial() {
    local name="$1"
    local experiment="$2"
    local input_distance="$3"
    local output_distance="$4"
    local producer="$5"
    local mvm="$6"
    local recombine="$7"
    local trial_dir="${OUTPUT_ROOT}/configurations/${name}"
    local tile_id
    local object
    local -A built_tiles=()

    mkdir -p -- "${trial_dir}"
    for tile_id in "${producer}" "${mvm}" "${recombine}"; do
        if [[ -n "${built_tiles[${tile_id}]:-}" ]]; then
            continue
        fi
        built_tiles["${tile_id}"]=1
        object="${trial_dir}/tile-${tile_id}.o"
        "${CLANGXX}" "${cxx_flags[@]}" \
            "-DMITTENS_TILE_ID=${tile_id}" \
            "-DMITTENS_PRODUCER_TILE=${producer}" \
            "-DMITTENS_MVM_TILE=${mvm}" \
            "-DMITTENS_RECOMBINE_TILE=${recombine}" \
            -c "${TEST_DIR}/main.cpp" \
            -o "${object}"
        "${CLANGXX}" "${common_flags[@]}" \
            -nostdlib -nostartfiles -nodefaultlibs \
            -fuse-ld=lld \
            -Wl,--build-id=none \
            -Wl,--gc-sections \
            "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
            "${COMMON_OUTPUT}/crt0.o" \
            "${COMMON_OUTPUT}/uart.o" \
            "${COMMON_OUTPUT}/platform-exit.o" \
            "${object}" \
            -o "${trial_dir}/tile-${tile_id}.elf"
    done
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${name}" "${experiment}" \
        "${input_distance}" "${output_distance}" \
        "${producer}" "${mvm}" "${recombine}" \
        >> "${CONFIGURATIONS}"
}

for distance in "${DISTANCES[@]}"; do
    build_trial \
        "activation-d${distance}" \
        activation-distance \
        "${distance}" 0 \
        0 "${distance}" "${distance}"
done

for distance in "${DISTANCES[@]}"; do
    build_trial \
        "partial-d${distance}" \
        partial-distance \
        0 "${distance}" \
        0 0 "$((distance * WIDTH))"
done

for input_distance in "${DISTANCES[@]}"; do
    for output_distance in "${DISTANCES[@]}"; do
        build_trial \
            "combined-in${input_distance}-out${output_distance}" \
            combined-distance \
            "${input_distance}" "${output_distance}" \
            0 \
            "${input_distance}" \
            "$((output_distance * WIDTH + input_distance))"
    done
done

echo "built producer-MVM-recombine distance images"
echo "${CONFIGURATIONS}"
