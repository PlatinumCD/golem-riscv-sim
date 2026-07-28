#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

readonly HOST_PYTHON="${COMPILER_HOST_PYTHON:-/usr/bin/python3}"
readonly PYTORCH_INDEX_URL="https://download.pytorch.org/whl/cpu"

require_executable "${HOST_PYTHON}"

if [[ ! -x "${COMPILER_PYTHON}" ]]; then
    "${HOST_PYTHON}" -m venv "${COMPILER_PYTHON_ENV}"
fi

"${COMPILER_PYTHON}" -m pip install \
    --disable-pip-version-check \
    --upgrade \
    "nanobind==${COMPILER_NANOBIND_VERSION}" \
    "numpy==${COMPILER_NUMPY_VERSION}" \
    "pybind11==${COMPILER_PYBIND11_VERSION}" \
    "PyYAML==${COMPILER_PYYAML_VERSION}" \
    "ml_dtypes==${COMPILER_ML_DTYPES_VERSION}" \
    "packaging==${COMPILER_PACKAGING_VERSION}" \
    "filelock==${COMPILER_FILELOCK_VERSION}" \
    "typing-extensions==${COMPILER_TYPING_EXTENSIONS_VERSION}" \
    "setuptools==${COMPILER_SETUPTOOLS_VERSION}" \
    "sympy==${COMPILER_SYMPY_VERSION}" \
    "networkx==${COMPILER_NETWORKX_VERSION}" \
    "Jinja2==${COMPILER_JINJA2_VERSION}" \
    "fsspec==${COMPILER_FSSPEC_VERSION}" \
    "mpmath==${COMPILER_MPMATH_VERSION}" \
    "MarkupSafe==${COMPILER_MARKUPSAFE_VERSION}"

"${COMPILER_PYTHON}" -m pip install \
    --disable-pip-version-check \
    --index-url "${PYTORCH_INDEX_URL}" \
    --no-deps \
    --upgrade \
    "torch==${PYTORCH_VERSION}"

"${COMPILER_PYTHON}" - <<'PY'
import torch

print(f"installed compiler Python environment with PyTorch {torch.__version__}")
PY
