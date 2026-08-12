#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly OUTPUT_ROOT="${GOLEM_COMPILER_SWEEP_OUTPUT_DIR:-${SCRIPT_DIR}/results/gpt2/two-decoder-compiler-sweep}"
readonly COST_PROFILE="${PROJECT_ROOT}/third_party/sculptor-mlir/tests/python_tests/data/calibrated_mapping_costs.json"
readonly SUMMARY="${OUTPUT_ROOT}/sweep-results.csv"

usage() {
    cat >&2 <<EOF
usage: $0 [--run] [--trace] [--trial NAME] [--force]
  --run         Run SST after each compiler trial.
  --trace       Record full traces. This option requires --run.
  --trial NAME  Run only the specified trial.
  --force       Replace completed trial output.
EOF
}

RUN_SIMULATION=false
TRACE_SIMULATION=false
SELECTED_TRIAL=""
FORCE=false
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --run)
            RUN_SIMULATION=true
            shift
            ;;
        --trace)
            TRACE_SIMULATION=true
            shift
            ;;
        --trial)
            if [[ "$#" -lt 2 ]]; then
                echo "--trial requires a trial name" >&2
                exit 2
            fi
            SELECTED_TRIAL="$2"
            shift 2
            ;;
        --force)
            FORCE=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done
readonly RUN_SIMULATION
readonly TRACE_SIMULATION
readonly SELECTED_TRIAL
readonly FORCE

if [[ "${TRACE_SIMULATION}" == true && "${RUN_SIMULATION}" != true ]]; then
    echo "--trace requires --run" >&2
    exit 2
fi

# Fields: name, data flow, depth, strict, reduction, minimum width, profile,
# placement objective, temporal network, timing scope, candidate limit, schedule.
readonly -a TRIALS=(
    "legacy-snake:bulk:0:false:none:3:legacy:transfer-cost:finite:warm:8:snake"
    "legacy-greedy:bulk:0:false:none:3:legacy:transfer-cost:finite:warm:8:greedy"
    "profile-transfer:bulk:0:false:none:3:test:transfer-cost:finite:warm:8:greedy"
    "makespan-ideal:bulk:0:false:none:3:test:makespan:ideal:warm:8:greedy"
    "makespan-finite:bulk:0:false:none:3:test:makespan:finite:warm:8:greedy"
    "makespan-full:bulk:0:false:none:3:test:makespan:full:warm:8:greedy"
    "makespan-cold:bulk:0:false:none:3:test:makespan:full:cold:8:greedy"
    "candidate-1:bulk:0:false:none:3:test:makespan:full:warm:1:greedy"
    "candidate-4:bulk:0:false:none:3:test:makespan:full:warm:4:greedy"
    "candidate-16:bulk:0:false:none:3:test:makespan:full:warm:16:greedy"
    "sharded-depth-0:sharded:0:false:none:3:legacy:transfer-cost:finite:warm:8:greedy"
    "sharded-depth-1:sharded:1:false:none:3:legacy:transfer-cost:finite:warm:8:greedy"
    "sharded-depth-2:sharded:2:false:none:3:legacy:transfer-cost:finite:warm:8:greedy"
    "sharded-strict:sharded:0:true:none:3:legacy:transfer-cost:finite:warm:8:greedy"
    "balanced-min-3:bulk:0:false:balanced:3:legacy:transfer-cost:finite:warm:8:greedy"
    "balanced-min-4:bulk:0:false:balanced:4:legacy:transfer-cost:finite:warm:8:greedy"
    "balanced-min-8:bulk:0:false:balanced:8:legacy:transfer-cost:finite:warm:8:greedy"
    "integrated-sharded:sharded:0:false:none:3:test:makespan:full:warm:8:greedy"
    "integrated-balanced:sharded:0:false:balanced:3:test:makespan:full:warm:8:greedy"
)

clear_directory() {
    local directory="$1"
    if [[ -d "${directory}" ]]; then
        find "${directory}" -mindepth 1 -depth -delete
    fi
}

run_trial() {
    local specification="$1"
    local name dataflow depth strict reduction minimum_width profile
    local objective network scope candidate schedule
    IFS=: read -r name dataflow depth strict reduction minimum_width profile \
        objective network scope candidate schedule <<<"${specification}"

    if [[ -n "${SELECTED_TRIAL}" && "${name}" != "${SELECTED_TRIAL}" ]]; then
        return 3
    fi

    local trial_directory="${OUTPUT_ROOT}/${name}"
    local resume_build=false
    if [[ "${FORCE}" != true ]]; then
        if [[ "${RUN_SIMULATION}" == true &&
              -f "${trial_directory}/.complete" &&
              ! -s "${trial_directory}/result.csv" ]]; then
            resume_build=true
        elif [[ -f "${trial_directory}/.complete" ]]; then
            echo "${name}: already complete"
            return 2
        elif [[ "${RUN_SIMULATION}" == true &&
                -f "${trial_directory}/.failed" ]]; then
            echo "${name}: skipping previous compiler failure"
            return 2
        fi
    fi

    if [[ "${resume_build}" != true ]]; then
        clear_directory "${trial_directory}"
        mkdir -p -- "${trial_directory}"
    fi

    local cost_profile=""
    if [[ "${profile}" == test ]]; then
        cost_profile="${COST_PROFILE}"
    fi

    printf '%s\n' \
        'name,dataflow,shard_depth,strict_shards,reduction_tree,reduction_min_width,cost_profile,placement_objective,network_mode,timing_scope,candidate_limit,schedule' \
        "${name},${dataflow},${depth},${strict},${reduction},${minimum_width},${profile},${objective},${network},${scope},${candidate},${schedule}" \
        >"${trial_directory}/trial-config.csv"

    local -a runner_options=(--remove-intermediate-mlir)
    if [[ "${RUN_SIMULATION}" == true ]]; then
        runner_options+=(--run)
    fi
    if [[ "${resume_build}" == true ]]; then
        runner_options+=(--resume-build)
    fi
    if [[ "${TRACE_SIMULATION}" == true ]]; then
        runner_options+=(--trace)
    fi

    echo "${name}: dataflow=${dataflow}, reduction=${reduction}, objective=${objective}, network=${network}"
    if GOLEM_EXPERIMENT_OUTPUT_DIR="${trial_directory}" \
       SCULPTOR_MESH_ROWS=10 \
       SCULPTOR_MESH_COLS=10 \
       GPT2_NUM_DECODERS=2 \
       GPT2_SEQUENCE_LENGTH=6 \
       SCULPTOR_PARALLEL_WORKERS=4 \
       SCULPTOR_DIGITAL_ISSUE_WIDTH=2 \
       SCULPTOR_NETWORK_WORD_BITS=32 \
       SCULPTOR_PLACEMENT_SCHEDULE="${schedule}" \
       SCULPTOR_DATAFLOW="${dataflow}" \
       SCULPTOR_SHARD_PROPAGATION_DEPTH="${depth}" \
       SCULPTOR_REQUIRE_COMPLETE_SHARD_CHAIN="${strict}" \
       SCULPTOR_REDUCTION_TREE="${reduction}" \
       SCULPTOR_REDUCTION_FAN_IN=2 \
       SCULPTOR_REDUCTION_MIN_WIDTH="${minimum_width}" \
       SCULPTOR_COST_PROFILE="${cost_profile}" \
       SCULPTOR_PLACEMENT_OBJECTIVE="${objective}" \
       SCULPTOR_TEMPORAL_NETWORK_MODE="${network}" \
       SCULPTOR_TIMING_SCOPE="${scope}" \
       SCULPTOR_TEMPORAL_CANDIDATE_LIMIT="${candidate}" \
       GOLEM_MODEL_PROFILE_MODE=summary \
       "${SCRIPT_DIR}/two-decoder-test.sh" "${runner_options[@]}" \
       2>&1 | tee "${trial_directory}/run.log"; then
        echo "${name}: complete"
        return 0
    fi

    date -u +'%Y-%m-%dT%H:%M:%SZ' >"${trial_directory}/.failed"
    echo "${name}: failed" >&2
    return 1
}

write_summary() {
    printf '%s\n' \
        'trial,status,dataflow,shard_depth,strict_shards,reduction_tree,reduction_min_width,cost_profile,placement_objective,network_mode,timing_scope,candidate_limit,schedule,predicted_makespan_ns,total_transfer_cost,simulated_time,wall_seconds' \
        >"${SUMMARY}"

    local specification name dataflow depth strict reduction minimum_width
    local profile objective network scope candidate schedule status
    local trial_directory placement_summary result predicted transfer simulated wall
    for specification in "${TRIALS[@]}"; do
        IFS=: read -r name dataflow depth strict reduction minimum_width profile \
            objective network scope candidate schedule <<<"${specification}"
        trial_directory="${OUTPUT_ROOT}/${name}"
        status="missing"
        [[ -f "${trial_directory}/.complete" ]] && status="complete"
        [[ -f "${trial_directory}/.failed" ]] && status="failed"
        predicted=""
        transfer=""
        simulated=""
        wall=""

        placement_summary="${trial_directory}/compiler/placement-summary.csv"
        if [[ -s "${placement_summary}" ]]; then
            read -r predicted transfer < <(
                awk -F, 'NR == 2 { print $23, $22 }' "${placement_summary}"
            )
        fi
        result="${trial_directory}/result.csv"
        if [[ -s "${result}" ]]; then
            simulated="$(awk -F, 'NR == 2 { print $(NF - 1) }' "${result}")"
            wall="$(awk -F, 'NR == 2 { print $NF }' "${result}")"
        fi

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${name}" "${status}" "${dataflow}" "${depth}" "${strict}" \
            "${reduction}" "${minimum_width}" "${profile}" "${objective}" \
            "${network}" "${scope}" "${candidate}" "${schedule}" \
            "${predicted}" "${transfer}" "${simulated}" "${wall}" \
            >>"${SUMMARY}"
    done
}

mkdir -p -- "${OUTPUT_ROOT}"
if [[ ! -s "${COST_PROFILE}" ]]; then
    echo "missing compiler cost profile: ${COST_PROFILE}" >&2
    exit 1
fi

failures=0
completed=0
skipped=0
selected=false
index=0
for trial in "${TRIALS[@]}"; do
    name="${trial%%:*}"
    if [[ -n "${SELECTED_TRIAL}" && "${name}" != "${SELECTED_TRIAL}" ]]; then
        continue
    fi
    selected=true
    ((index += 1))
    echo "[${index}] ${name}"
    set +e
    run_trial "${trial}"
    status=$?
    set -e
    case "${status}" in
        0) ((completed += 1)) ;;
        1) ((failures += 1)) ;;
        2) ((skipped += 1)) ;;
        3) ;;
    esac
    write_summary
done

if [[ "${selected}" != true ]]; then
    echo "unknown trial: ${SELECTED_TRIAL}" >&2
    exit 2
fi

echo "compiler sweep: ${completed} completed, ${skipped} skipped, ${failures} failed"
echo "summary: ${SUMMARY}"
if ((failures > 0)); then
    exit 1
fi
