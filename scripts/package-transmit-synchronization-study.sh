#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly BUILD_ROOT="${PROJECT_ROOT}/build"
readonly ARTIFACT_ROOT="${BUILD_ROOT}/research-artifacts"
readonly OUTPUT="${ARTIFACT_ROOT}/transmit-synchronization"
readonly ARCHIVE="${ARTIFACT_ROOT}/transmit-synchronization.tar.gz"
readonly FANOUT_RESULTS="${BUILD_ROOT}/tests/transmit-fanout/results"
readonly GPT_ROOT="${BUILD_ROOT}/tests/sculptor-gpt2-8x8"

required=(
    "${FANOUT_RESULTS}/polling-results.csv"
    "${FANOUT_RESULTS}/blocking-results.csv"
    "${FANOUT_RESULTS}/overlap-blocking-results.csv"
    "${FANOUT_RESULTS}/async-results.csv"
    "${GPT_ROOT}/deployment/performance-profile/summary.json"
    "${GPT_ROOT}/deployment-blocking-trace/performance-profile/summary.json"
    "${GPT_ROOT}/deployment-doorbell-blocking-trace/performance-profile/summary.json"
    "${GPT_ROOT}/deployment-doorbell-async-trace/performance-profile/summary.json"
)
for path in "${required[@]}"; do
    if [[ ! -f "${path}" ]]; then
        echo "missing required study result: ${path}" >&2
        exit 1
    fi
done

"${SCRIPT_DIR}/plot-transmit-synchronization-study.py"

mkdir -p -- "${ARTIFACT_ROOT}"
temporary="$(mktemp -d "${ARTIFACT_ROOT}/transmit-sync.tmp.XXXXXX")"
trap 'rm -rf -- "${temporary}"' EXIT
mkdir -p -- \
    "${temporary}/fanout" \
    "${temporary}/gpt2" \
    "${temporary}/source"

cp -a -- "${FANOUT_RESULTS}/." "${temporary}/fanout/"
for entry in \
    "polling:deployment" \
    "blocking-notification:deployment-blocking-trace" \
    "doorbell-blocking:deployment-doorbell-blocking-trace" \
    "doorbell-async:deployment-doorbell-async-trace"; do
    label="${entry%%:*}"
    directory="${entry#*:}"
    mkdir -p -- "${temporary}/gpt2/${label}"
    cp -a -- \
        "${GPT_ROOT}/${directory}/performance-profile" \
        "${GPT_ROOT}/${directory}/performance-profile-raw" \
        "${GPT_ROOT}/${directory}/deployment-routes.csv" \
        "${GPT_ROOT}/${directory}/router-statistics.csv" \
        "${GPT_ROOT}/${directory}/simulation.log" \
        "${temporary}/gpt2/${label}/"
done

cp -a -- \
    "${PROJECT_ROOT}/tests/transmit-fanout" \
    "${PROJECT_ROOT}/tests/sculptor-gpt2-8x8" \
    "${PROJECT_ROOT}/scripts/analyze-performance-profile.py" \
    "${PROJECT_ROOT}/scripts/plot-transmit-synchronization-study.py" \
    "${PROJECT_ROOT}/docs/research-results/transmit-synchronization" \
    "${temporary}/source/"
mkdir -p -- \
    "${temporary}/source/implementation/components/devices" \
    "${temporary}/source/implementation/components/elements" \
    "${temporary}/source/implementation/platform"
cp -a -- \
    "${PROJECT_ROOT}/bridge" \
    "${PROJECT_ROOT}/runtime" \
    "${temporary}/source/implementation/"
cp -a -- \
    "${PROJECT_ROOT}/components/devices/mittens-nic" \
    "${PROJECT_ROOT}/components/devices/mittens-sync" \
    "${temporary}/source/implementation/components/devices/"
cp -a -- \
    "${PROJECT_ROOT}/components/elements/mittens" \
    "${temporary}/source/implementation/components/elements/"
cp -a -- \
    "${PROJECT_ROOT}/platform/mesh-nic.h" \
    "${temporary}/source/implementation/platform/"

# Never package interpreter cache state from a machine that generated the
# artifact. These exact directories live only inside the temporary package.
while IFS= read -r -d '' cache; do
    rm -rf -- "${cache}"
done < <(find "${temporary}/source" -type d -name __pycache__ -print0)

{
    echo "study=transmit-synchronization"
    echo "repository_commit=$(git -C "${PROJECT_ROOT}" rev-parse HEAD)"
    echo "qemu_commit=$(git -C "${PROJECT_ROOT}/third_party/qemu" rev-parse HEAD)"
    echo "sculptor_commit=$(git -C "${PROJECT_ROOT}/third_party/sculptor-mlir" rev-parse HEAD)"
    echo "llvm_commit=$(git -C "${PROJECT_ROOT}/third_party/llvm-project" rev-parse HEAD)"
    echo "sst_core_commit=$(git -C "${PROJECT_ROOT}/third_party/sst-core" rev-parse HEAD)"
    echo "sst_elements_commit=$(git -C "${PROJECT_ROOT}/third_party/sst-elements" rev-parse HEAD)"
    echo "host=$(uname -a)"
    echo "qemu=$("${PROJECT_ROOT}/install/qemu/bin/qemu-system-riscv64" --version | head -1)"
    echo "sst=$("${PROJECT_ROOT}/install/sst-core/bin/sst" --version | head -1)"
} >"${temporary}/manifest.txt"

(
    cd -- "${temporary}"
    find . -type f ! -name SHA256SUMS -print0 \
        | sort -z \
        | xargs -0 sha256sum \
        >SHA256SUMS
    sha256sum -c SHA256SUMS >/dev/null
)

if [[ -e "${OUTPUT}" ]]; then
    rm -rf -- "${OUTPUT}"
fi
mv -- "${temporary}" "${OUTPUT}"
trap - EXIT
tar -C "${ARTIFACT_ROOT}" -czf "${ARCHIVE}" \
    "$(basename -- "${OUTPUT}")"
tar -tzf "${ARCHIVE}" >/dev/null

echo "study directory: ${OUTPUT}"
echo "study archive: ${ARCHIVE}"
