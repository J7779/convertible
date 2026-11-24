#!/usr/bin/env bash
# Runs the web app's end-to-end tests: the real app in a real headless
# Chrome/Chromium, driven over the DevTools protocol by an in-house driver
# (Node 22+, no packages). Builds apps/web/dist first if it is missing.
#
#   apps/web/e2e.sh                       # whole suite
#   apps/web/e2e.sh --test-name-pattern=offline
#
# Environment:
#   CHROME_BIN            browser to drive (default: found in the usual places)
#   E2E_REQUIRE_BROWSER=1 fail instead of skipping when no browser is found
#   E2E_CONCURRENCY       test files run in parallel (default 2)
#   E2E_LONG_MB           size of the inputs of the cancel tests (default 192)
#   CI                    when set, Chrome runs with --no-sandbox
#   CVT_WEB_DIST          serve another build directory
#   CVT_E2E_BUILD         build.sh mode when dist is missing (default --docker)
set -euo pipefail

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${CVT_WEB_DIST:-${WEB_DIR}/dist}"

node_major="$(node -p 'process.versions.node.split(".")[0]')"
if (( node_major < 22 )); then
  echo "error: the end-to-end tests need Node.js 22 or newer (found $(node --version))" >&2
  exit 1
fi

if [[ ! -f "${DIST_DIR}/wasm/convertible.wasm" ]]; then
  echo "==> no build in ${DIST_DIR#"${WEB_DIR}/"}; building it first"
  "${WEB_DIR}/build.sh" "${CVT_E2E_BUILD:---docker}"
fi

exec node --test \
  --test-reporter=spec \
  --test-concurrency="${E2E_CONCURRENCY:-2}" \
  "$@" \
  "${WEB_DIR}"/e2e/*.test.mjs
