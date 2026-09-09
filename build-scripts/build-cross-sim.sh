#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SUBMODULE="${PROJECT_ROOT}/third_party/cross-sim"
readonly SOURCE="${PREPARED_SOURCE_ROOT}/cross-sim"
readonly PYTHON="${CROSSSIM_PYTHON:-/usr/bin/python3}"
require_owned_comparison_output "${CROSSSIM_SITE_PACKAGES}" "${INSTALL_ROOT}"
require_owned_comparison_output "${SOURCE}" "${PREPARED_SOURCE_ROOT}"

for command in find install; do
    require_command "${command}"
done
require_executable "${PYTHON}"
prepare_worktree "${SUBMODULE}" "${SOURCE}" "${CROSSSIM_COMMIT}" "CrossSim"

if ! "${PYTHON}" -m pip --version >/dev/null 2>&1; then
    echo "CrossSim requires pip for ${PYTHON}" >&2
    exit 1
fi

mkdir -p -- "${CROSSSIM_SITE_PACKAGES}"

"${PYTHON}" -m pip install \
    --disable-pip-version-check \
    --upgrade \
    --target "${CROSSSIM_SITE_PACKAGES}" \
    "numpy==${CROSSSIM_NUMPY_VERSION}" \
    "scipy==${CROSSSIM_SCIPY_VERSION}"

"${PYTHON}" -m pip install \
    --disable-pip-version-check \
    --no-build-isolation \
    --no-deps \
    --upgrade \
    --target "${CROSSSIM_SITE_PACKAGES}" \
    "${SOURCE}"

# CrossSim 3.2.1's wheel omits simulator/configs/*.json even though
# CrossSimParameters.from_json() resolves built-in names from that directory.
while IFS= read -r -d '' configuration; do
    install -D -m 0644 \
        "${configuration}" \
        "${CROSSSIM_SITE_PACKAGES}/simulator/configs/$(basename -- "${configuration}")"
done < <(find "${SOURCE}/simulator/configs" -maxdepth 1 -type f \
    -name '*.json' -print0)

PYTHONPATH="${CROSSSIM_SITE_PACKAGES}${PYTHONPATH:+:${PYTHONPATH}}" \
    "${PYTHON}" - <<'PY'
import numpy
import scipy
import simulator

assert hasattr(simulator, "AnalogCore")
assert hasattr(simulator, "CrossSimParameters")
simulator.CrossSimParameters.from_json("default")
print(
    "installed CrossSim with "
    f"NumPy {numpy.__version__} and SciPy {scipy.__version__}"
)
PY

echo "installed CrossSim: ${CROSSSIM_SITE_PACKAGES}"
