#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../../support/test-env.sh
source "${TEST_DIR}/../../support/test-env.sh"

readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly OUTPUT_ROOT="${TEST_RESULTS_ROOT}/network-timing-validation"
readonly TRIALS="${OUTPUT_ROOT}/trials.tsv"
readonly RESULTS="${OUTPUT_ROOT}/results.csv"
readonly ANALYZER="${TEST_DIR}/analyze-results.py"

require_executable "${SST}"
require_executable "${ANALYZER}"
require_file "${INSTALL_ROOT}/sst-elements/lib/sst-elements-library/libmittens.so"

mkdir -p -- "${OUTPUT_ROOT}"
printf 'experiment\tconfiguration\twidth\theight\tsources\tdestination\tpayload_words\texpected_hops\tsource_count\traw_path\tstats_path\n' \
    >"${TRIALS}"

run_trial() {
    local experiment="$1"
    local configuration="$2"
    local width="$3"
    local height="$4"
    local sources="$5"
    local destination="$6"
    local words="$7"
    local expected_hops="$8"
    local trial="${OUTPUT_ROOT}/${experiment}/${configuration}"
    local raw="${trial}/receipts.csv"
    local stats="${trial}/router-statistics.csv"
    local log="${trial}/simulation.log"
    local source_count

    source_count="$(
        awk -F, '{ print NF }' <<<"${sources}"
    )"
    mkdir -p -- "${trial}"
    rm -f -- "${raw}" "${stats}" "${log}"

    printf '[network timing] experiment=%s config=%s words=%s hops=%s sources=%s\n' \
        "${experiment}" \
        "${configuration}" \
        "${words}" \
        "${expected_hops}" \
        "${source_count}"

    MITTENS_NETWORK_TIMING_WIDTH="${width}" \
    MITTENS_NETWORK_TIMING_HEIGHT="${height}" \
    MITTENS_NETWORK_TIMING_SOURCES="${sources}" \
    MITTENS_NETWORK_TIMING_DESTINATION="${destination}" \
    MITTENS_NETWORK_TIMING_PAYLOAD_WORDS="${words}" \
    MITTENS_NETWORK_TIMING_OUTPUT="${raw}" \
    MITTENS_NETWORK_TIMING_STATS="${stats}" \
    SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}" \
        "${SST}" "${TEST_DIR}/simulation.py" 2>&1 |
        tee "${log}"

    require_file "${raw}"
    require_file "${stats}"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${experiment}" \
        "${configuration}" \
        "${width}" \
        "${height}" \
        "${sources}" \
        "${destination}" \
        "${words}" \
        "${expected_hops}" \
        "${source_count}" \
        "${raw}" \
        "${stats}" \
        >>"${TRIALS}"
}

for words in 1 8 64 512; do
    run_trial \
        serialization \
        "words-${words}" \
        2 1 0 1 "${words}" 1
done

for hop_count in 1 2 4; do
    run_trial \
        distance \
        "hops-${hop_count}" \
        5 1 0 "${hop_count}" 64 "${hop_count}"
done

for words in 1 64; do
    run_trial contention "words-${words}-sources-1" 3 3 1 4 "${words}" 1
    run_trial contention "words-${words}-sources-2" 3 3 1,3 4 "${words}" 1
    run_trial contention "words-${words}-sources-4" 3 3 1,3,5,7 4 "${words}" 1
done

"${ANALYZER}" "${TRIALS}" "${RESULTS}"
echo "isolated SST mesh timing validation: PASS"
