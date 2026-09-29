#!/usr/bin/env bash
set -euo pipefail
echo 'This legacy runtime/platform builder is retired. Use bash tests/run-all.sh --suite compiler for current single-tile compiler execution; see docs/migration.md.' >&2
exit 2
