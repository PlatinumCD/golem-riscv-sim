#!/usr/bin/env bash
set -euo pipefail

readonly TESTS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly GROUP="${1:?usage: tests/run-group.sh GROUP}"

if [[ "$#" -ne 1 ]]; then
    echo "run-group.sh accepts one test group" >&2
    exit 2
fi

case "${GROUP}" in
    platform|compiler|runtime|memory|network|analog|models|validation)
        ;;
    *)
        echo "unknown test group: ${GROUP}" >&2
        echo "groups: platform compiler runtime memory network analog models validation" >&2
        exit 2
        ;;
esac

readonly GROUP_DIR="${TESTS_ROOT}/${GROUP}"
test_count=0
for test_dir in "${GROUP_DIR}"/*; do
    [[ -d "${test_dir}" ]] || continue
    test_name="${GROUP}/$(basename -- "${test_dir}")"
    if [[ -x "${test_dir}/run-all.sh" ]]; then
        echo "[test] ${test_name}"
        "${test_dir}/run-all.sh"
        ((test_count += 1))
    elif [[ -x "${test_dir}/run-test.sh" ]]; then
        echo "[test] ${test_name}"
        "${test_dir}/run-test.sh"
        ((test_count += 1))
    fi
done

if [[ "${test_count}" -eq 0 ]]; then
    echo "test group has no runnable entries: ${GROUP}" >&2
    exit 1
fi

echo "test group ${GROUP}: PASS (${test_count} entries)"
