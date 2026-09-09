#!/usr/bin/env bash

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "common.sh must be sourced, not executed" >&2
    exit 2
fi

readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
hardware_path_exports="$(python3 "${PROJECT_ROOT}/tools/hardware/hardware_paths.py" --shell)" || return 2
eval "${hardware_path_exports}"
unset hardware_path_exports
readonly HARDWARE_TREE="${GOLEM_HARDWARE_TREE}"
readonly HARDWARE_ROOT="${PROJECT_ROOT}/src"
readonly BUILD_ROOT="${GOLEM_BUILD_ROOT}"
readonly INSTALL_ROOT="${GOLEM_INSTALL_ROOT}"
readonly PREPARED_SOURCE_ROOT="${GOLEM_SOURCE_ROOT}"
readonly TEST_RESULTS_ROOT="${GOLEM_TEST_RESULTS_ROOT:-${PROJECT_ROOT}/tests/results}"

printf 'Build scope: %s\nBuild: %s\nInstall: %s\nPrepared sources: %s\nLLVM: %s\n' \
    "${GOLEM_BUILD_SCOPE:-hardware}" "${BUILD_ROOT}" "${INSTALL_ROOT}" \
    "${PREPARED_SOURCE_ROOT}" "${GOLEM_LLVM_DIR}" >&2

require_sculptor_source() {
    if [[ -z "${GOLEM_SCULPTOR_SOURCE:-}" || ! -f "${GOLEM_SCULPTOR_SOURCE}/CMakeLists.txt" ]]; then
        echo 'Optional compiler/runtime requires GOLEM_SCULPTOR_SOURCE pointing to an external Sculptor checkout (not a submodule).' >&2
        return 1
    fi
}

require_owned_comparison_output() {
    [[ "${HARDWARE_TREE}" == src ]] || return 0
    local destination="$1" owner="$2"
    case "$(realpath -m -- "${destination}")" in
        "$(realpath -m -- "${owner}")/"*) return 0 ;;
        *) echo "comparison output escapes its owner through a symlink: ${destination}" >&2; return 2 ;;
    esac
}
readonly CROSSSIM_SITE_PACKAGES="${INSTALL_ROOT}/cross-sim/python"
readonly COMPILER_PYTHON_ENV="${INSTALL_ROOT}/compiler-python"
readonly COMPILER_PYTHON="${COMPILER_PYTHON_ENV}/bin/python"

if [[ -n "${JOBS:-}" ]]; then
    readonly BUILD_JOBS="${JOBS}"
else
    readonly BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)"
fi

readonly CONFIG_BUILD_ROOT="${HARDWARE_ROOT}/config/build"
readonly QEMU_DEVICE_ROOT="${HARDWARE_ROOT}/qemu/devices"
readonly QEMU_INSTRUCTION_ROOT="${HARDWARE_ROOT}/qemu/instructions"
readonly SST_ELEMENT_ROOT="${HARDWARE_ROOT}/sst"
readonly PLATFORM_ROOT="${HARDWARE_ROOT}/platform/devices"
readonly PLATFORM_STARTUP_ROOT="${HARDWARE_ROOT}/platform/startup"
readonly PLATFORM_RUNTIME_ROOT="${HARDWARE_ROOT}/platform/runtime"
source "${CONFIG_BUILD_ROOT}/versions.env"
# shellcheck source=../src/config/toolchain.env
source "${CONFIG_BUILD_ROOT}/toolchain.env"

# Resolve implementation files without flattening src's ownership directories.
platform_source() {
    local name="$1" directory
    for directory in "${PLATFORM_ROOT}" "${PLATFORM_STARTUP_ROOT}" "${PLATFORM_RUNTIME_ROOT}"; do
        if [[ -f "${directory}/${name}" ]]; then
            printf '%s\n' "${directory}/${name}"
            return 0
        fi
    done
    echo "unknown platform source: ${name}" >&2
    return 1
}

# Convert the integer byte-size forms used by linker scripts and SST (for
# example 67108864, 64M, 64MiB, or 64MB) to an integer byte count.
parse_byte_size() {
    local value="${1^^}"
    local number_text prefix unit multiplier number

    if [[ ! "${value}" =~ ^([0-9]+)([KMGT]?)(I?B)?$ ]]; then
        echo "invalid byte size: $1" >&2
        return 2
    fi
    number_text="${BASH_REMATCH[1]}"
    prefix="${BASH_REMATCH[2]}"
    unit="${BASH_REMATCH[3]}"
    if [[ -n "${unit}" && "${unit}" != B && "${unit}" != IB ]]; then
        echo "invalid byte-size unit: $1" >&2
        return 2
    fi
    case "${prefix}" in
        "") multiplier=1 ;;
        K) multiplier=1024 ;;
        M) multiplier=1048576 ;;
        G) multiplier=1073741824 ;;
        T) multiplier=1099511627776 ;;
        *)
            echo "invalid byte-size prefix: $1" >&2
            return 2
            ;;
    esac
    number=$((10#${number_text}))
    if ((number > 9223372036854775807 / multiplier)); then
        echo "byte size exceeds the supported integer range: $1" >&2
        return 2
    fi
    printf '%d\n' "$((number * multiplier))"
}

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
