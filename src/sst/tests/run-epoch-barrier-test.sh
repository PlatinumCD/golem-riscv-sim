#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly TEST_PROJECT_ROOT="$(cd -- "${TEST_DIR}/../../.." && pwd)"
export GOLEM_HARDWARE_TREE=src
source "${TEST_PROJECT_ROOT}/build-scripts/common.sh"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_DIR="${BUILD_ROOT}/tests/mittens-epoch-barrier"
readonly SERIAL_LOG="${OUTPUT_DIR}/serial.log"
readonly PARALLEL_LOG="${OUTPUT_DIR}/parallel.log"
readonly PREFIX_LOG="${OUTPUT_DIR}/prefix.log"

if [[ ! -x "${SST}" || ! -d "${ELEMENT_LIBRARY}" ]]; then
    echo "build SST Core and the Mittens element before this test" >&2
    exit 1
fi

mkdir -p -- "${OUTPUT_DIR}"
export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"

rm -f -- "${SERIAL_LOG}" "${PARALLEL_LOG}" "${PREFIX_LOG}"
"${SST}" "${TEST_DIR}/epoch_barrier.py" >"${SERIAL_LOG}" 2>&1
"${SST}" -n 2 --partitioner=sst.simple \
    "${TEST_DIR}/epoch_barrier.py" >"${PARALLEL_LOG}" 2>&1
MITTENS_EPOCH_BARRIER_STOP_AFTER_RELEASES=2 \
    "${SST}" "${TEST_DIR}/epoch_barrier.py" >"${PREFIX_LOG}" 2>&1

for log in "${SERIAL_LOG}" "${PARALLEL_LOG}"; do
    if [[ "$(grep -c 'MITTENS_EPOCH_BARRIER_RELEASE' "${log}")" -ne 3 ]] ||
       [[ "$(grep -c 'MITTENS_EPOCH_BARRIER_PROBE' "${log}")" -ne 9 ]]; then
        echo "epoch barrier did not release three epochs to three active tiles: ${log}" >&2
        exit 1
    fi
    grep -F 'completed_epoch=0 released_epoch=1 arrivals=3 idle=1' \
        "${log}" >/dev/null
    grep -F 'completed_epoch=1 released_epoch=2 arrivals=3 idle=1' \
        "${log}" >/dev/null
    grep -F 'completed_epoch=2 released_epoch=3 arrivals=3 idle=1' \
        "${log}" >/dev/null
done

if [[ "$(grep -c 'MITTENS_EPOCH_BARRIER_RELEASE' "${PREFIX_LOG}")" -ne 2 ]]; then
    echo "epoch barrier prefix gate did not stop after two releases" >&2
    exit 1
fi
grep -F \
    'MITTENS_EPOCH_PREFIX_COMPLETE releases=2 epoch_count=3' \
    "${PREFIX_LOG}" >/dev/null
if grep -F 'epoch barrier simulation ended incomplete' \
    "${PREFIX_LOG}" >/dev/null; then
    echo "intentional epoch prefix was reported as an incomplete run" >&2
    exit 1
fi

grep -E 'MITTENS_EPOCH_BARRIER_PROBE .*sst_thread=0' \
    "${PARALLEL_LOG}" >/dev/null
grep -E 'MITTENS_EPOCH_BARRIER_PROBE .*sst_thread=1' \
    "${PARALLEL_LOG}" >/dev/null

for mode in duplicate future stale; do
    invalid_log="${OUTPUT_DIR}/invalid-${mode}.log"
    rm -f -- "${invalid_log}"
    if MITTENS_EPOCH_BARRIER_INVALID="${mode}" \
        "${SST}" "${TEST_DIR}/epoch_barrier.py" \
        >"${invalid_log}" 2>&1; then
        echo "epoch barrier unexpectedly accepted ${mode} arrival" >&2
        exit 1
    fi
    grep -F "epoch barrier ${mode} arrival:" "${invalid_log}" >/dev/null
done

echo "modeled deployment epoch barrier: PASS (serial, two-thread, clean prefix stop, idle, duplicate/stale/future diagnostics)"
