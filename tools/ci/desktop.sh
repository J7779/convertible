#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Build, test and package the desktop app (apps/desktop) as the CI desktop jobs do, on
# Linux, macOS or Windows (Git Bash). Packages land in build/desktop/packages/.
#
#   tools/ci/desktop.sh                 # configure + build + desktop tests + package
#   CVT_DESKTOP_TESTS=0 tools/ci/desktop.sh      # release build: no tests
#   CVT_CMAKE_ARGS='-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' tools/ci/desktop.sh
#
# The core's own unit tests are not built here (the core jobs run them); the desktop tests
# (CTest label "desktop") and, when present, the end-to-end suite (label "desktop-e2e")
# are. On Linux the end-to-end suite runs under Xvfb when xvfb-run is installed.
# Environment: CVT_JOBS, CVT_CMAKE_ARGS (one extra argument per line or space separated),
#              CVT_DESKTOP_TESTS (default 1), CVT_BUILD_DIR (default build/desktop).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

if [[ ! -f apps/desktop/CMakeLists.txt ]]; then
  echo "desktop: apps/desktop is not in this checkout; nothing to do"
  exit 0
fi

build="${CVT_BUILD_DIR:-build/desktop}"
jobs="${CVT_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")}"
tests="${CVT_DESKTOP_TESTS:-1}"
desktop_tests="ON"
[[ "${tests}" == "1" ]] || desktop_tests="OFF"

extra=()
if [[ -n "${CVT_CMAKE_ARGS:-}" ]]; then
  # One argument per line keeps values with spaces or semicolons intact.
  while IFS= read -r line; do [[ -n "${line}" ]] && extra+=("${line}"); done <<< "${CVT_CMAKE_ARGS// -D/$'\n'-D}"
fi

echo "==> configure ${build}"
cmake -S . -B "${build}" -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release \
  -DCVT_BUILD_TESTS=OFF -DCVT_BUILD_CLI=ON -DCVD_BUILD_TESTS="${desktop_tests}" ${extra[@]+"${extra[@]}"}
echo "==> build (-j ${jobs})"
cmake --build "${build}" -j "${jobs}" --config Release

if [[ "${tests}" == "1" ]]; then
  echo "==> desktop tests"
  ctest --test-dir "${build}" -L '^desktop$' -C Release --output-on-failure --timeout 1800
  if ctest --test-dir "${build}" --print-labels -C Release | grep -q 'desktop-e2e'; then
    echo "==> desktop end-to-end tests"
    if [[ "$(uname -s)" == "Linux" ]] && command -v xvfb-run >/dev/null 2>&1; then
      xvfb-run -a -s "-screen 0 1600x1000x24" \
        ctest --test-dir "${build}" -L '^desktop-e2e$' -C Release --output-on-failure --timeout 1800
    else
      ctest --test-dir "${build}" -L '^desktop-e2e$' -C Release --output-on-failure --timeout 1800
    fi
  else
    echo "==> no desktop-e2e tests registered; skipped"
  fi
fi

echo "==> package"
cmake --build "${build}" --config Release --target package
ls -l "${build}/packages"
