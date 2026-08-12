#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly OUTPUT_ROOT="${GOLEM_NETWORK_SWEEP_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/two-decoder-network-sweep}"
readonly SHARED_BUILD="${OUTPUT_ROOT}/shared-build"
readonly TRIAL_ROOT="${OUTPUT_ROOT}/trials"
readonly SUMMARY="${OUTPUT_ROOT}/sweep-results.csv"

readonly -a TRIALS=(
    "link-32:link-width:32:16:16"
    "link-64:link-width:64:16:16"
    "link-128:link-width:128:16:16"
    "link-256:link-width:256:16:16"
    "buffer-32:router-buffer:32:32:16"
    "buffer-64:router-buffer:32:64:16"
    "buffer-128:router-buffer:32:128:16"
    "packet-32:packet-size:32:128:32"
    "packet-64:packet-size:32:128:64"
)

clear_directory() {
    local directory="$1"
    if [[ -d "${directory}" ]]; then
        find "${directory}" -mindepth 1 -depth -delete
    fi
}

prepare_shared_build() {
    if [[ -s "${SHARED_BUILD}/active-cores.txt" &&
          -d "${SHARED_BUILD}/compiler/cores" ]]; then
        echo "reusing shared two-decoder compiler output"
        return
    fi

    echo "building the shared two-decoder deployment"
    GOLEM_EXPERIMENT_OUTPUT_DIR="${SHARED_BUILD}" \
        "${SCRIPT_DIR}/two-decoder-test.sh" --remove-intermediate-mlir
}

snapshot_trial() {
    local trial_directory="$1"
    mkdir -p -- "${trial_directory}"
    cp -a \
        "${SHARED_BUILD}/result.csv" \
        "${SHARED_BUILD}/router-statistics.csv" \
        "${SHARED_BUILD}/simulation.log" \
        "${SHARED_BUILD}/parameters.env" \
        "${SHARED_BUILD}/active-cores.txt" \
        "${trial_directory}/"
    cp -a "${SHARED_BUILD}/trace" "${trial_directory}/trace"
    date -u +'%Y-%m-%dT%H:%M:%SZ' >"${trial_directory}/.complete"
}

run_trial() {
    local specification="$1"
    local name dimension width buffer packet
    IFS=: read -r name dimension width buffer packet <<<"${specification}"
    local trial_directory="${TRIAL_ROOT}/${name}"

    if [[ -f "${trial_directory}/.complete" ]]; then
        echo "${name}: already complete"
        return
    fi

    clear_directory "${trial_directory}"
    clear_directory "${SHARED_BUILD}/trace"

    echo "${name}: width=${width} bits, buffer=${buffer} words, packet=${packet} words"
    GOLEM_EXPERIMENT_OUTPUT_DIR="${SHARED_BUILD}" \
    SCULPTOR_PLACEMENT_SCHEDULE=snake \
    SCULPTOR_NETWORK_WORD_BITS="${width}" \
    GOLEM_MODEL_NETWORK_BUFFER_CELLS="${buffer}" \
    GOLEM_MODEL_NETWORK_PACKET_WORDS="${packet}" \
        "${SCRIPT_DIR}/two-decoder-test.sh" \
            --run --trace --resume-build

    snapshot_trial "${trial_directory}"
}

write_summary() {
    printf '%s\n' \
        'trial,dimension,link_width_bits,buffer_words,packet_words,simulated_time,wall_seconds' \
        >"${SUMMARY}"
    local specification name dimension width buffer packet result
    for specification in "${TRIALS[@]}"; do
        IFS=: read -r name dimension width buffer packet <<<"${specification}"
        result="${TRIAL_ROOT}/${name}/result.csv"
        if [[ ! -s "${result}" ]]; then
            continue
        fi
        awk -F, -v name="${name}" -v dimension="${dimension}" \
            -v width="${width}" -v buffer="${buffer}" -v packet="${packet}" \
            'NR == 2 { print name "," dimension "," width "," buffer "," packet "," $11 "," $12 }' \
            "${result}" >>"${SUMMARY}"
    done
}

mkdir -p -- "${OUTPUT_ROOT}" "${TRIAL_ROOT}"
prepare_shared_build

index=0
for trial in "${TRIALS[@]}"; do
    ((index += 1))
    echo "[${index}/${#TRIALS[@]}]"
    run_trial "${trial}"
done

write_summary
echo "network sweep complete: ${SUMMARY}"
