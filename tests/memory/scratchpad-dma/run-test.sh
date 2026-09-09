#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/output.txt"
readonly BATCH_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/batch-output.txt"
readonly MACRO_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/macro-output.txt"
readonly INVALID_MACRO_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/invalid-macro-output.txt"
readonly BURST_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/burst-output.txt"
readonly QUANTUM_SCALAR_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/quantum-scalar-output.txt"
readonly QUANTUM_MACRO_OUTPUT="${TEST_RESULTS_ROOT}/scratchpad-dma/quantum-macro-output.txt"
readonly SCALAR_PROFILE_DIRECTORY="${TEST_RESULTS_ROOT}/scratchpad-dma/profile-scalar"
readonly SCALAR_STATISTICS="${TEST_RESULTS_ROOT}/scratchpad-dma/scalar-statistics.csv"
readonly MACRO_PROFILE_DIRECTORY="${TEST_RESULTS_ROOT}/scratchpad-dma/profile-macro"
readonly QUANTUM_SCALAR_PROFILE_DIRECTORY="${TEST_RESULTS_ROOT}/scratchpad-dma/profile-quantum-scalar"
readonly QUANTUM_MACRO_PROFILE_DIRECTORY="${TEST_RESULTS_ROOT}/scratchpad-dma/profile-quantum-macro"
readonly SCALAR_NORMALIZED_WAITS="${TEST_RESULTS_ROOT}/scratchpad-dma/scalar-waits-normalized.csv"
readonly MACRO_NORMALIZED_WAITS="${TEST_RESULTS_ROOT}/scratchpad-dma/macro-waits-normalized.csv"

rm -rf -- \
    "${SCALAR_PROFILE_DIRECTORY}" \
    "${MACRO_PROFILE_DIRECTORY}" \
    "${QUANTUM_SCALAR_PROFILE_DIRECTORY}" \
    "${QUANTUM_MACRO_PROFILE_DIRECTORY}"
mkdir -p -- \
    "${SCALAR_PROFILE_DIRECTORY}" \
    "${MACRO_PROFILE_DIRECTORY}" \
    "${QUANTUM_SCALAR_PROFILE_DIRECTORY}" \
    "${QUANTUM_MACRO_PROFILE_DIRECTORY}"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" scratchpad-dma
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_TEST_ELF="${TEST_RESULTS_ROOT}/scratchpad-dma/scratchpad-dma.elf"

MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY="${SCALAR_PROFILE_DIRECTORY}" \
MITTENS_TEST_STATISTICS="${SCALAR_STATISTICS}" \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${OUTPUT}"
grep -F "global DMA test: PASS" "${OUTPUT}"
grep -Eq \
    'stop_memory_init=1 .*memory_init_handshakes=1 memory_init_accesses=6 memory_init_read_bytes=0 memory_init_write_bytes=48 memory_init_cycles=4 ' \
    "${OUTPUT}"
python3 - \
    "${SCALAR_PROFILE_DIRECTORY}/tile-0-summary.csv" \
    "${SCALAR_STATISTICS}" <<'PY'
import csv
import sys

with open(sys.argv[1], encoding="utf-8", newline="") as source:
    summary = {
        row["metric"]: int(row["value"])
        for row in csv.DictReader(source)
    }
with open(sys.argv[2], encoding="utf-8", newline="") as source:
    statistics = {
        (row["ComponentName"], row["StatisticName"]): int(row["Sum.u64"])
        for row in csv.DictReader(source)
    }
service = statistics.get(("tile0", "scratchpad_service_cycles"))
assert service is not None and service > 0, service
assert summary.get("scratchpad_service_cycles") == service, (
    summary.get("scratchpad_service_cycles"), service
)
PY

MITTENS_TEST_GLOBAL_DMA_SUBMIT_BATCHING=1 \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${BATCH_OUTPUT}"
grep -F "global DMA test: PASS" "${BATCH_OUTPUT}"
grep -Eq \
    'stop_dma_wait_batch=1 dma_wait_batch_records=8' \
    "${BATCH_OUTPUT}"

MITTENS_TEST_GLOBAL_DMA_MACRO_EXECUTION=1 \
MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY="${MACRO_PROFILE_DIRECTORY}" \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" |
    tee "${MACRO_OUTPUT}"
grep -F "global DMA test: PASS" "${MACRO_OUTPUT}"
grep -Eq 'dma_macro_records=8' "${MACRO_OUTPUT}"

# The macro is a Class-A host-transport optimization.  Its complete modeled
# summary and logical wait timeline must remain byte-identical to scalar mode;
# only the fd-41 event sequence is removed from the normalized trace.
cmp \
    "${SCALAR_PROFILE_DIRECTORY}/tile-0-summary.csv" \
    "${MACRO_PROFILE_DIRECTORY}/tile-0-summary.csv"
cmp \
    "${SCALAR_PROFILE_DIRECTORY}/global-ram-requests.csv" \
    "${MACRO_PROFILE_DIRECTORY}/global-ram-requests.csv"
cut -d, --complement -f2 \
    "${SCALAR_PROFILE_DIRECTORY}/tile-0-waits.csv" \
    > "${SCALAR_NORMALIZED_WAITS}"
cut -d, --complement -f2 \
    "${MACRO_PROFILE_DIRECTORY}/tile-0-waits.csv" \
    > "${MACRO_NORMALIZED_WAITS}"
cmp "${SCALAR_NORMALIZED_WAITS}" "${MACRO_NORMALIZED_WAITS}"

scalar_scratchpad_profile="$(
    grep -F 'MITTENS_SCRATCHPAD_PROFILE tile=0 ' "${OUTPUT}" |
        sed -E 's/dma_macro_records=[0-9]+/dma_macro_records=<transport>/'
)"
macro_scratchpad_profile="$(
    grep -F 'MITTENS_SCRATCHPAD_PROFILE tile=0 ' "${MACRO_OUTPUT}" |
        sed -E 's/dma_macro_records=[0-9]+/dma_macro_records=<transport>/'
)"
if [[ -z "${scalar_scratchpad_profile}" ||
      "${scalar_scratchpad_profile}" != "${macro_scratchpad_profile}" ]]; then
    echo "global DMA macro changed scratchpad accounting" >&2
    diff -u \
        <(printf '%s\n' "${scalar_scratchpad_profile}") \
        <(printf '%s\n' "${macro_scratchpad_profile}") || true
    exit 1
fi

scalar_last_wait_event="$(
    awk -F, 'NR > 1 && $2 + 0 > maximum { maximum = $2 + 0 }
             END { print maximum }' \
        "${SCALAR_PROFILE_DIRECTORY}/tile-0-waits.csv"
)"
macro_last_wait_event="$(
    awk -F, 'NR > 1 && $2 + 0 > maximum { maximum = $2 + 0 }
             END { print maximum }' \
        "${MACRO_PROFILE_DIRECTORY}/tile-0-waits.csv"
)"
if [[ -z "${scalar_last_wait_event}" || -z "${macro_last_wait_event}" ||
      $((scalar_last_wait_event - macro_last_wait_event)) -ne 14 ]]; then
    echo "global DMA macro did not remove exactly 14 fd-41 envelopes" >&2
    exit 1
fi

# A semantic event inside a compiler-certified macro must fail closed.  The
# invalid fixture performs one timed scratchpad access between begin and end;
# accepting it would make the macro trace observably incomplete.
export MITTENS_TEST_ELF="${TEST_RESULTS_ROOT}/scratchpad-dma/scratchpad-dma-invalid-macro.elf"
set +e
MITTENS_TEST_GLOBAL_DMA_MACRO_EXECUTION=1 \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${INVALID_MACRO_OUTPUT}" 2>&1
invalid_macro_status=$?
set -e
if [[ ${invalid_macro_status} -eq 0 ]]; then
    echo "global DMA macro accepted an intervening semantic event" >&2
    exit 1
fi
grep -F \
    "QEMU synchronization device reported bridge protocol error 2" \
    "${INVALID_MACRO_OUTPUT}"

# If the compiler's conservative instruction span cannot fit in the current
# icount grant, macro begin must become an invisible scalar fallback.  A small
# initialization grant forces the macro sites into 1000-instruction grants,
# below this fixture's certified 65536-instruction bound.
export MITTENS_TEST_ELF="${TEST_RESULTS_ROOT}/scratchpad-dma/scratchpad-dma.elf"
MITTENS_TEST_MEMORY_INIT_INSTRUCTION_QUANTUM=512 \
MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY="${QUANTUM_SCALAR_PROFILE_DIRECTORY}" \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${QUANTUM_SCALAR_OUTPUT}"
MITTENS_TEST_MEMORY_INIT_INSTRUCTION_QUANTUM=512 \
MITTENS_TEST_GLOBAL_DMA_MACRO_EXECUTION=1 \
MITTENS_TEST_PROFILE_OUTPUT_DIRECTORY="${QUANTUM_MACRO_PROFILE_DIRECTORY}" \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${QUANTUM_MACRO_OUTPUT}"
grep -F "global DMA test: PASS" "${QUANTUM_SCALAR_OUTPUT}"
grep -F "global DMA test: PASS" "${QUANTUM_MACRO_OUTPUT}"
grep -F "dma_macro_records=0" "${QUANTUM_MACRO_OUTPUT}"
cmp \
    "${QUANTUM_SCALAR_PROFILE_DIRECTORY}/tile-0-summary.csv" \
    "${QUANTUM_MACRO_PROFILE_DIRECTORY}/tile-0-summary.csv"
cmp \
    "${QUANTUM_SCALAR_PROFILE_DIRECTORY}/global-ram-requests.csv" \
    "${QUANTUM_MACRO_PROFILE_DIRECTORY}/global-ram-requests.csv"
cut -d, --complement -f2 \
    "${QUANTUM_SCALAR_PROFILE_DIRECTORY}/tile-0-waits.csv" \
    > "${SCALAR_NORMALIZED_WAITS}.quantum"
cut -d, --complement -f2 \
    "${QUANTUM_MACRO_PROFILE_DIRECTORY}/tile-0-waits.csv" \
    > "${MACRO_NORMALIZED_WAITS}.quantum"
cmp \
    "${SCALAR_NORMALIZED_WAITS}.quantum" \
    "${MACRO_NORMALIZED_WAITS}.quantum"

export MITTENS_BURST_STATS="${TEST_RESULTS_ROOT}/scratchpad-dma/burst-stats.csv"
"${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/burst-simulation.py" |
    tee "${BURST_OUTPUT}"
if [[ "$(grep -Fc "global DMA test: PASS" "${BURST_OUTPUT}")" -ne 17 ]]; then
    echo "global DMA burst did not complete all 17 endpoints" >&2
    exit 1
fi
