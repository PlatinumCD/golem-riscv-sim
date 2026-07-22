#!/usr/bin/env bash
# Build the matching SST Elements release against this repository's SST Core.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE="$ROOT/third_party/sst-elements"
readonly CORE_INSTALL="${SST_CORE_INSTALL_DIR:-$ROOT/build/sst-core-v16-install}"
readonly BUILD="${SST_ELEMENTS_BUILD_DIR:-$ROOT/build/sst-elements-v16}"
readonly INSTALL="${SST_ELEMENTS_INSTALL_DIR:-$ROOT/build/sst-elements-v16-install}"
readonly JOBS="${JOBS:-$(nproc)}"

need() { command -v "$1" >/dev/null || { echo "Missing required command: $1" >&2; exit 1; }; }
for tool in autoconf automake libtoolize make python3; do need "$tool"; done

if [[ "$(git -C "$SOURCE" describe --exact-match --tags HEAD 2>/dev/null || true)" != "v16.0.0_Final" ]]; then
  echo "Expected third_party/sst-elements at v16.0.0_Final." >&2
  exit 1
fi
if [[ ! -x "$CORE_INSTALL/bin/sst-config" ]]; then
  echo "SST Core is not installed; run ./bootstrap.sh sst-core first." >&2
  exit 1
fi

(cd "$SOURCE" && ./autogen.sh)
mkdir -p "$BUILD"
cd "$BUILD"
"$SOURCE/configure" \
  --prefix="$INSTALL" \
  --with-sst-core="$CORE_INSTALL" \
  --disable-picky-warnings
make -j "$JOBS"
make install
