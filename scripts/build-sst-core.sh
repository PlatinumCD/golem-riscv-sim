#!/usr/bin/env bash
# Build the pinned SST Core release with host-specific release optimization.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly SOURCE="$ROOT/third_party/sst-core"
readonly BUILD="${SST_CORE_BUILD_DIR:-$ROOT/build/sst-core-v16}"
readonly INSTALL="${SST_CORE_INSTALL_DIR:-$ROOT/build/sst-core-v16-install}"
readonly JOBS="${JOBS:-$(nproc)}"

need() { command -v "$1" >/dev/null || { echo "Missing required command: $1" >&2; exit 1; }; }
for tool in autoconf automake libtoolize make python3 mpicc mpicxx; do need "$tool"; done

if [[ "$(git -C "$SOURCE" describe --exact-match --tags HEAD 2>/dev/null || true)" != "v16.0.0_Final" ]]; then
  echo "Expected third_party/sst-core at v16.0.0_Final." >&2
  exit 1
fi

# This installation is deliberately specialized for this AArch64 host.
export CFLAGS="${CFLAGS:--O3 -march=native -DNDEBUG}"
export CXXFLAGS="${CXXFLAGS:--O3 -march=native -DNDEBUG}"

(cd "$SOURCE" && ./autogen.sh)
mkdir -p "$BUILD"
cd "$BUILD"
"$SOURCE/configure" --prefix="$INSTALL" --disable-picky-warnings
make -j "$JOBS"
make install
