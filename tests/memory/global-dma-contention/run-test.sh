#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"
readonly OUTPUT_DIR="${TEST_RESULTS_ROOT}/global-dma-contention"
readonly SCALAR_LOG="${OUTPUT_DIR}/scalar.log"
readonly SCALAR_STATS="${OUTPUT_DIR}/scalar-statistics.csv"
readonly SCALAR_PROFILE="${OUTPUT_DIR}/scalar-profile"
readonly SCALAR_UART="${OUTPUT_DIR}/scalar-uart"

"${PROJECT_ROOT}/build-scripts/build-platform.sh" \
    global-dma-contention
export SST_LIB_PATH="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
export MITTENS_GLOBAL_DMA_CONTENTION_TILE0="${OUTPUT_DIR}/tile0.elf"
export MITTENS_GLOBAL_DMA_CONTENTION_TILE1="${OUTPUT_DIR}/tile1.elf"

mkdir -p -- "${SCALAR_PROFILE}" "${SCALAR_UART}"

export MITTENS_GLOBAL_DMA_CONTENTION_STATS="${SCALAR_STATS}"
export MITTENS_GLOBAL_DMA_CONTENTION_PROFILE="${SCALAR_PROFILE}"
export MITTENS_GLOBAL_DMA_CONTENTION_UART="${SCALAR_UART}"
timeout --foreground --signal=TERM --kill-after=5s 60s \
    "${INSTALL_ROOT}/sst-core/bin/sst" "${TEST_DIR}/simulation.py" \
    > "${SCALAR_LOG}" 2>&1

for tile in 0 1; do
    grep -F "DMA contention tile ${tile}: PASS" "${SCALAR_UART}/tile-${tile}.log"
done
python3 - "${SCALAR_PROFILE}/global-ram-requests.csv" "${SCALAR_STATS}" <<'CHECK'
import csv
import sys
with open(sys.argv[1]) as stream: all_requests=list(csv.DictReader(stream))
boot=[r for r in all_requests if int(r['execution_id'])==0]
requests=[r for r in all_requests if int(r['execution_id'])==97]
assert boot and all(r['direction']=='read' for r in boot)
assert len(requests)==4 and {int(r['tile_id']) for r in requests}=={0,1}
assert all(int(r['byte_count'])==4096 for r in requests)
reads=[r for r in requests if r['direction']=='read']
assert len(reads)==2 and all(int(r['readiness_wait_cycles'])>0 for r in reads)
with open(sys.argv[2]) as stream:
    stats={r['StatisticName']:int(r['Sum.u64']) for r in csv.DictReader(stream) if r['ComponentName']=='global_ram'}
expected={'requests':len(all_requests),'bytes':sum(int(r['byte_count']) for r in all_requests),
          'readiness_blocked_reads':2,'readiness_releases':2,'readiness_publications':2,
          'readiness_duplicate_publications':0,'readiness_maximum_waiters':2,
          'readiness_execution_teardowns':1}
for name,value in expected.items(): assert stats[name]==value,(name,stats[name],value)
print('Shared DMA contention: PASS (two early reads, two publications, exact payloads, clean teardown)')
CHECK
