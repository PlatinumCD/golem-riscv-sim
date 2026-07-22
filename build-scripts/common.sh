#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "common.sh must be sourced, not executed" >&2
    exit 2
fi

readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT:-${PROJECT_ROOT}/build}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT:-${PROJECT_ROOT}/install}"
readonly PREPARED_SOURCE_ROOT="${GOLEM_SOURCE_ROOT:-${BUILD_ROOT}/sources}"

if [[ -n "${JOBS:-}" ]]; then
    readonly BUILD_JOBS="${JOBS}"
else
    readonly BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)"
fi

# shellcheck source=../config/versions.env
source "${PROJECT_ROOT}/config/versions.env"
# shellcheck source=../config/toolchain.env
source "${PROJECT_ROOT}/config/toolchain.env"

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        return 1
    fi
}

require_executable() {
    if [[ ! -x "$1" ]]; then
        echo "missing required executable: $1" >&2
        return 1
    fi
}

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "missing required file: $1" >&2
        return 1
    fi
}

require_git_commit() {
    local repository="$1"
    local expected="$2"
    local label="$3"
    local actual

    if ! actual="$(git -C "${repository}" rev-parse HEAD 2>/dev/null)"; then
        echo "${label} is not initialized: ${repository}" >&2
        return 1
    fi
    if [[ "${actual}" != "${expected}" ]]; then
        echo "${label} is at ${actual}; expected ${expected}" >&2
        return 1
    fi
}

require_clean_submodule() {
    local repository="$1"
    local label="$2"

    if [[ -n "$(git -C "${repository}" status --porcelain)" ]]; then
        echo "${label} submodule has local changes: ${repository}" >&2
        return 1
    fi
}

prepare_worktree() {
    local submodule="$1"
    local destination="$2"
    local expected="$3"
    local label="$4"
    local actual

    require_git_commit "${submodule}" "${expected}" "${label}"
    require_clean_submodule "${submodule}" "${label}"

    if [[ ! -e "${destination}" ]]; then
        mkdir -p -- "$(dirname -- "${destination}")"
        git -C "${submodule}" worktree add --detach "${destination}" "${expected}"
    fi

    if ! actual="$(git -C "${destination}" rev-parse HEAD 2>/dev/null)"; then
        echo "prepared source is not a Git worktree: ${destination}" >&2
        return 1
    fi
    if [[ "${actual}" != "${expected}" ]]; then
        echo "prepared ${label} source is at ${actual}; expected ${expected}" >&2
        echo "remove only ${destination} and rerun the preparation script" >&2
        return 1
    fi
}

apply_patch_once() {
    local source_tree="$1"
    local patch_file="$2"

    require_file "${patch_file}"
    if git -C "${source_tree}" apply --check "${patch_file}" 2>/dev/null; then
        git -C "${source_tree}" apply "${patch_file}"
    elif git -C "${source_tree}" apply --reverse --check "${patch_file}" \
        2>/dev/null; then
        return 0
    else
        echo "patch does not apply cleanly: ${patch_file}" >&2
        return 1
    fi
}
