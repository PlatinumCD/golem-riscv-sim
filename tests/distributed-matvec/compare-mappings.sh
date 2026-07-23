#!/usr/bin/env bash
set -euo pipefail

readonly TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${TEST_DIR}/../.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly BASELINE_BUILD="${BUILD_ROOT}/tests/distributed-matvec"
readonly REVERSE_BUILD="${BUILD_ROOT}/tests/distributed-matvec-reverse"
readonly SST="${INSTALL_ROOT}/sst-core/bin/sst"
readonly ELEMENT_LIBRARY="${INSTALL_ROOT}/sst-elements/lib/sst-elements-library"
readonly QEMU="${INSTALL_ROOT}/qemu/bin/qemu-system-riscv64"
readonly SIMULATION="${TEST_DIR}/simulation.py"
readonly COMPARISON_RUNS="${RUNS:-10}"

if [[ ! "${COMPARISON_RUNS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "RUNS must be a positive integer" >&2
    exit 2
fi

for executable in "${SST}" "${QEMU}"; do
    if [[ ! -x "${executable}" ]]; then
        echo "missing required executable: ${executable}" >&2
        echo "run ${PROJECT_ROOT}/bootstrap.sh build first" >&2
        exit 1
    fi
done

"${PROJECT_ROOT}/build-scripts/build-platform.sh" distributed-matvec
"${PROJECT_ROOT}/build-scripts/build-platform.sh" distributed-matvec-reverse

export SST_LIB_PATH="${ELEMENT_LIBRARY}${SST_LIB_PATH:+:${SST_LIB_PATH}}"
export MITTENS_TEST_QEMU="${QEMU}"

run_mapping() {
    local mapping="$1"
    local image_dir="$2"
    local statistics="$3"
    local output
    local value
    local unit

    export MITTENS_MATVEC_STATS="${statistics}"
    for tile_id in {0..8}; do
        export \
            "MITTENS_MATVEC_TILE${tile_id}_ELF=${image_dir}/tile${tile_id}.elf"
    done

    output="$("${SST}" "${SIMULATION}")"
    if [[ "$(grep -c 'executed task' <<<"${output}")" -ne 13 ]] ||
        ! grep -q \
            'all 13 tasks complete; final vector \[398,82,120,243\]' \
            <<<"${output}"; then
        echo "${mapping} mapping failed" >&2
        return 1
    fi

    value="$(
        awk '/Simulation is complete, simulated time:/ { print $(NF - 1) }' \
            <<<"${output}"
    )"
    unit="$(
        awk '/Simulation is complete, simulated time:/ { print $NF }' \
            <<<"${output}"
    )"
    case "${unit}" in
        s) value="$(awk -v value="${value}" 'BEGIN { print value * 1000 }')" ;;
        ms) ;;
        us) value="$(awk -v value="${value}" 'BEGIN { print value / 1000 }')" ;;
        ns) value="$(awk -v value="${value}" 'BEGIN { print value / 1000000 }')" ;;
        *)
            echo "unexpected SST time unit: ${unit}" >&2
            return 1
            ;;
    esac

    echo "${mapping},${value}"
}

verify_network_statistics() {
    local mapping="$1"
    local statistics="$2"
    local expected_words="$3"
    local expected_word_hops="$4"

    if ! awk -F, \
        -v expected_words="${expected_words}" \
        -v expected_word_hops="${expected_word_hops}" '
            $1 ~ /^router_/ &&
            $2 == "send_packet_count" &&
            $3 == "port4" {
                delivered_words += $7
            }
            $1 ~ /^router_/ &&
            $2 == "send_packet_count" &&
            $3 ~ /^port[0-3]$/ {
                directional_word_hops += $7
            }
            $1 ~ /^router_/ &&
            $2 == "send_bit_count" &&
            $3 ~ /^port[0-3]$/ {
                directional_bit_hops += $7
            }
            END {
                valid = delivered_words == expected_words && directional_word_hops == expected_word_hops && directional_bit_hops == expected_word_hops * 32
                exit valid ? 0 : 1
            }
        ' "${statistics}"; then
        echo "${mapping} mapping network statistics are incorrect" >&2
        return 1
    fi
}

baseline_statistics="${BASELINE_BUILD}/comparison-statistics.csv"
reverse_statistics="${REVERSE_BUILD}/comparison-statistics.csv"

{
    for ((run = 1; run <= COMPARISON_RUNS; ++run)); do
        if ((run % 2 == 1)); then
            run_mapping baseline "${BASELINE_BUILD}" "${baseline_statistics}"
            run_mapping reverse "${REVERSE_BUILD}" "${reverse_statistics}"
        else
            run_mapping reverse "${REVERSE_BUILD}" "${reverse_statistics}"
            run_mapping baseline "${BASELINE_BUILD}" "${baseline_statistics}"
        fi
    done
} | gawk -F, '
    {
        sample_count[$1] += 1
        values[$1, sample_count[$1]] = $2
        sum[$1] += $2
        if (sample_count[$1] == 1 || $2 < minimum[$1]) {
            minimum[$1] = $2
        }
        if (sample_count[$1] == 1 || $2 > maximum[$1]) {
            maximum[$1] = $2
        }
    }
    END {
        mappings[1] = "baseline"
        mappings[2] = "reverse"
        for (mapping_index = 1; mapping_index <= 2; ++mapping_index) {
            mapping = mappings[mapping_index]
            delete sorted
            for (sample = 1; sample <= sample_count[mapping]; ++sample) {
                sorted[sample] = values[mapping, sample]
            }
            asort(sorted)
            middle = int((sample_count[mapping] + 1) / 2)
            if (sample_count[mapping] % 2 == 0) {
                median = (sorted[middle] + sorted[middle + 1]) / 2
            } else {
                median = sorted[middle]
            }
            printf \
                "%s: runs=%u mean=%.4f ms median=%.4f ms " \
                "min=%.4f ms max=%.4f ms\n", \
                mapping, \
                sample_count[mapping], \
                sum[mapping] / sample_count[mapping], \
                median, \
                minimum[mapping], \
                maximum[mapping]
        }
    }
'

verify_network_statistics baseline "${baseline_statistics}" 52 88
verify_network_statistics reverse "${reverse_statistics}" 48 80

echo "baseline network: 52 words, 88 directional word-hops"
echo "reverse network: 48 words, 80 directional word-hops"
