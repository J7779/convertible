#!/usr/bin/env bash
# Runs the web app's tests headlessly with Node's built-in test runner
# (Node 18+; no packages). Needs a build in apps/web/dist (apps/web/build.sh).
#
#   apps/web/test.sh                 # all tests
#   apps/web/test.sh --build         # build first (container or local emsdk)
#   CVT_WEB_DIST=/path/to/dist apps/web/test.sh
set -euo pipefail

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" == "--build" ]]; then
  "${WEB_DIR}/build.sh"
  shift
fi

if [[ ! -f "${CVT_WEB_DIST:-${WEB_DIR}/dist}/wasm/convertible.wasm" ]]; then
  echo "error: no build in ${CVT_WEB_DIST:-apps/web/dist}; run apps/web/build.sh first (or pass --build)" >&2
  exit 1
fi

exec node --test --test-reporter=spec "$@" "${WEB_DIR}"/tests/*.test.mjs
