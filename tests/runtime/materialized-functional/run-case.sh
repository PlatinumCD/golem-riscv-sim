#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 2 ]]; then
    echo "usage: $0 <case> <compiler-artifact-directory>" >&2
    exit 2
fi

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly CASE_NAME="$1"
readonly ARTIFACT_DIR="$(realpath -- "$2")"
readonly CASE_MATRIX="${TEST_DIR}/cases.json"
readonly ACTIVE_CORES="${ARTIFACT_DIR}/active-cores.txt"
readonly DEPLOYMENT_MANIFEST="${ARTIFACT_DIR}/deployment-manifest.json"
readonly CORE_DIR="${ARTIFACT_DIR}/cores"
readonly RUN_DIR="${MITTENS_MATERIALIZED_RUN_DIR:-${TEST_RESULTS_ROOT}/materialized-functional/${CASE_NAME}/run}"
readonly LLVM="${INSTALL_ROOT}/llvm/bin"
readonly RUNTIME_INCLUDE="${INSTALL_ROOT}/runtime/include"
readonly RUNTIME_LIBRARY="${INSTALL_ROOT}/runtime/lib/libgolem-runtime.a"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly LOG="${RUN_DIR}/simulation.log"
readonly STATISTICS="${RUN_DIR}/statistics.csv"

IFS=$'\t' read -r CASE_ID MESH_WIDTH MESH_HEIGHT DIGITAL_WORKERS MINIMUM_WORK \
    < <(python3 "${TEST_DIR}/case_config.py" "${CASE_MATRIX}" "${CASE_NAME}")
readonly CASE_ID MESH_WIDTH MESH_HEIGHT DIGITAL_WORKERS MINIMUM_WORK

for path in "${ACTIVE_CORES}" "${DEPLOYMENT_MANIFEST}" \
    "${RUNTIME_LIBRARY}"; do
    require_file "${path}"
done
for executable in "${LLVM}/clang" "${LLVM}/clang++" \
    "${LLVM}/llvm-readelf" "${QEMU}" "${SST}"; do
    require_executable "${executable}"
done

mapfile -t active_tiles < <(sort -n -u -- "${ACTIVE_CORES}")
if [[ "${#active_tiles[@]}" -eq 0 ]] ||
   [[ "$(wc -l <"${ACTIVE_CORES}")" -ne "${#active_tiles[@]}" ]]; then
    echo "active-core manifest must be nonempty and duplicate-free" >&2
    exit 1
fi
readonly SEED_TILE="${active_tiles[0]}"

python3 - "${PROJECT_ROOT}" "${DEPLOYMENT_MANIFEST}" \
    "$((MESH_WIDTH * MESH_HEIGHT))" "${active_tiles[@]}" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tests" / "support"))
from deployment_manifest import load_deployment_manifest
load_deployment_manifest(
    sys.argv[2], network_size=int(sys.argv[3]),
    expected_active_tiles=[int(value) for value in sys.argv[4:]],
)
PY

mkdir -p -- "${RUN_DIR}"
rm -f -- "${RUN_DIR}"/*.o "${RUN_DIR}"/*.elf "${RUN_DIR}"/*.log \
    "${STATISTICS}"

common_flags=(
    "--target=${GOLEM_TARGET}" "-mcpu=${GOLEM_CPU}" "-mabi=${GOLEM_ABI}"
    -mcmodel=medany -ffreestanding -fno-stack-protector
    -ffunction-sections -fdata-sections -O2
)
cxx_flags=(
    "${common_flags[@]}" -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
    -fno-asynchronous-unwind-tables -Wall -Wextra -Wpedantic -Werror
    "-I${RUNTIME_INCLUDE}" "-I${PLATFORM_ROOT}" "-I${TEST_DIR}"
)

"${LLVM}/clang" "${common_flags[@]}" -c "${PLATFORM_STARTUP_ROOT}/crt0.S" \
    -o "${RUN_DIR}/crt0.o"
for source in uart platform-exit freestanding-memory freestanding-math; do
    "${LLVM}/clang++" "${cxx_flags[@]}" \
        -c "$(platform_source "${source}.cpp")" \
        -o "${RUN_DIR}/${source}.o"
done
"${LLVM}/clang++" "${cxx_flags[@]}" \
    "-DMITTENS_MATERIALIZED_CASE=${CASE_ID}" \
    -c "${TEST_DIR}/case_oracle.cpp" -o "${RUN_DIR}/case-oracle.o"
"${LLVM}/clang++" "${cxx_flags[@]}" \
    -c "${TEST_DIR}/abi_accounting.cpp" -o "${RUN_DIR}/abi-accounting.o"
"${LLVM}/clang++" "${cxx_flags[@]}" \
    "-DMITTENS_MATERIALIZED_CASE=${CASE_ID}" \
    "-DMITTENS_MATERIALIZED_SEED_TILE=${SEED_TILE}" \
    -c "${TEST_DIR}/main.cpp" -o "${RUN_DIR}/main.o"
"${LLVM}/clang++" "${cxx_flags[@]}" \
    -c "${PLATFORM_ROOT}/sculptor-tile-abi-main.cpp" \
    -o "${RUN_DIR}/abi-main.o"
"${LLVM}/clang++" "${cxx_flags[@]}" \
    "-DMITTENS_MATERIALIZED_CASE=${CASE_ID}" \
    -c "${TEST_DIR}/preflight.cpp" -o "${RUN_DIR}/functional-abi-main.o"

link_elf() {
    local output="$1"
    local main_object="$2"
    local generated_object="$3"
    "${LLVM}/clang++" "${common_flags[@]}" -nostdlib -nostartfiles \
        -nodefaultlibs -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
        "-Wl,-T,${PLATFORM_STARTUP_ROOT}/tile.ld" \
        "${RUN_DIR}/crt0.o" "${RUN_DIR}/uart.o" \
        "${RUN_DIR}/platform-exit.o" "${RUN_DIR}/freestanding-memory.o" \
        "${RUN_DIR}/freestanding-math.o" "${main_object}" \
        "${RUN_DIR}/case-oracle.o" "${RUN_DIR}/abi-accounting.o" \
        "${generated_object}" \
        "${RUNTIME_LIBRARY}" -o "${output}"
    "${LLVM}/llvm-readelf" -h "${output}" | grep -F 'Machine:' | \
        grep -F 'RISC-V' >/dev/null
}

deployment_epoch_count="$(python3 - "${PROJECT_ROOT}" \
    "${DEPLOYMENT_MANIFEST}" "$((MESH_WIDTH * MESH_HEIGHT))" \
    "${active_tiles[@]}" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tests" / "support"))
from deployment_manifest import load_deployment_manifest
print(load_deployment_manifest(
    sys.argv[2], network_size=int(sys.argv[3]),
    expected_active_tiles=[int(value) for value in sys.argv[4:]],
)["epoch_count"])
PY
)"

for tile in "${active_tiles[@]}"; do
    object="${CORE_DIR}/core-${tile}.o"
    require_file "${object}"
    preflight="${RUN_DIR}/preflight-${tile}.elf"
    preflight_log="${RUN_DIR}/preflight-${tile}.log"
    link_elf "${preflight}" "${RUN_DIR}/abi-main.o" "${object}"
    timeout --foreground --signal=TERM --kill-after=2s 20s \
        "${QEMU}" -machine virt -cpu "${QEMU_RISCV_CPU}" -smp 1 \
        -m 16M -bios none -kernel "${preflight}" -display none -monitor none \
        -serial stdio -no-reboot </dev/null >"${preflight_log}" 2>&1
    expected="SCULPTOR_RA_ABI_PASS tile=${tile} epoch_count=${deployment_epoch_count}"
    if [[ "$(awk -v expected="${expected}" \
        '$0 == expected || $0 == expected "\r" {++count} END {print count + 0}' \
        "${preflight_log}")" -ne 1 ]]; then
        echo "tile ${tile} did not pass exact generated-ABI preflight" >&2
        sed -n '1,40p' "${preflight_log}" >&2
        exit 1
    fi
    functional_preflight="${RUN_DIR}/functional-preflight-${tile}.elf"
    functional_preflight_log="${RUN_DIR}/functional-preflight-${tile}.log"
    link_elf "${functional_preflight}" \
        "${RUN_DIR}/functional-abi-main.o" "${object}"
    timeout --foreground --signal=TERM --kill-after=2s 20s \
        "${QEMU}" -machine virt -cpu "${QEMU_RISCV_CPU}" -smp 1 \
        -m 16M -bios none -kernel "${functional_preflight}" \
        -display none -monitor none -serial stdio -no-reboot \
        </dev/null >"${functional_preflight_log}" 2>&1
    python3 - "${functional_preflight_log}" "${CASE_NAME}" \
        "${tile}" "${deployment_epoch_count}" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").replace("\r", "")
pattern = re.compile(
    rf"MATERIALIZED_FUNCTIONAL_PREFLIGHT case={re.escape(sys.argv[2])} "
    rf"tile={int(sys.argv[3])} epoch_count={int(sys.argv[4])} "
    r"iterations=[1-9][0-9]* full_requests=[1-9][0-9]* "
    r"tail_requests=[1-9][0-9]* runtime_requests=[1-9][0-9]* "
    r"runtime_bytes=[1-9][0-9]*"
)
matches = pattern.findall(text)
if len(matches) != 1 or "MATERIALIZED_FUNCTIONAL_PREFLIGHT_ERROR" in text:
    raise SystemExit("generated functional ABI did not pass exact 4KiB/tail preflight")
PY
    link_elf "${RUN_DIR}/tile-${tile}.elf" "${RUN_DIR}/main.o" "${object}"
done

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_MATERIALIZED_ELF_DIR="${RUN_DIR}"
export MITTENS_MATERIALIZED_ACTIVE_CORES="${ACTIVE_CORES}"
export MITTENS_MATERIALIZED_DEPLOYMENT_MANIFEST="${DEPLOYMENT_MANIFEST}"
export MITTENS_MATERIALIZED_MESH_WIDTH="${MESH_WIDTH}"
export MITTENS_MATERIALIZED_MESH_HEIGHT="${MESH_HEIGHT}"
export MITTENS_MATERIALIZED_STATS="${STATISTICS}"

set +e
timeout --foreground --signal=TERM --kill-after=5s 120s \
    "${SST}" "${TEST_DIR}/simulation.py" </dev/null 2>&1 | tee "${LOG}"
pipeline_status=("${PIPESTATUS[@]}")
set -e
if [[ "${pipeline_status[0]}" -ne 0 ]]; then
    echo "SST materialized ${CASE_NAME} exited with status ${pipeline_status[0]}" >&2
    exit "${pipeline_status[0]}"
fi
if [[ "${pipeline_status[1]}" -ne 0 ]]; then
    echo "failed to record the materialized ${CASE_NAME} SST log" >&2
    exit "${pipeline_status[1]}"
fi
python3 "${TEST_DIR}/validate_run.py" \
    --case "${CASE_NAME}" --log "${LOG}" --statistics "${STATISTICS}" \
    --active-cores "${ACTIVE_CORES}"
