#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly TEST_BUILD="${BUILD_ROOT}/tests/distributed-matvec"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly STATISTICS="${TEST_BUILD}/router-statistics.csv"
readonly OUTPUT="$(mktemp)"

trap 'rm -f -- "${OUTPUT}"' EXIT

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" distributed-matvec

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"
export MITTENS_MATVEC_STATS="${STATISTICS}"

for tile_id in {0..8}; do
    export "MITTENS_MATVEC_TILE${tile_id}_ELF=${TEST_BUILD}/tile${tile_id}.elf"
done

rm -f -- "${STATISTICS}"
"${SST}" "${TEST_DIR}/simulation.py" 2>&1 | tee "${OUTPUT}"

if [[ "$(grep -c 'executed task' "${OUTPUT}")" -ne 13 ]]; then
    echo "distributed matvec did not execute exactly 13 tasks" >&2
    exit 1
fi

if ! grep -q \
    'all 13 tasks complete; final vector \[398,82,120,243\]' \
    "${OUTPUT}"; then
    echo "distributed matvec did not produce the expected final vector" >&2
    exit 1
fi

if [[ ! -s "${STATISTICS}" ]]; then
    echo "distributed matvec did not produce router statistics" >&2
    exit 1
fi

if ! awk -F, '
    $2 == "send_packet_count" && $3 == "port4" {
        words += $7
    }
    END {
        exit !(words == 52)
    }
' "${STATISTICS}"; then
    echo "expected exactly 52 ejected 32-bit words" >&2
    exit 1
fi

if ! awk -F, '
    $2 == "send_bit_count" && $3 == "port4" {
        bits += $7
        records += 1
        if ($10 != 32 || $11 != 32) {
            invalid_width = 1
        }
    }
    END {
        exit !(bits == 1664 && records == 9 && !invalid_width)
    }
' "${STATISTICS}"; then
    echo "network statistics do not describe 32-bit transfers" >&2
    exit 1
fi

echo "distributed 13-task matrix-vector pipeline passed"
echo "network proof: 13 vectors x 4 float32 words = 52 words"
echo "router statistics: ${STATISTICS}"
