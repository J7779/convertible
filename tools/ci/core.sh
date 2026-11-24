#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Configure, build and test the core with one CMake preset (dev, asan or release), exactly
# as the CI core jobs do. Compiler selection comes from the environment (CC/CXX, CXXFLAGS,
# LDFLAGS), so the same script serves GCC, Clang + libc++, Apple Clang and MSVC.
#
#   tools/ci/core.sh dev                     # Debug + tests + CLI in build/dev
#   tools/ci/core.sh asan                    # AddressSanitizer + UBSan in build/asan
#   CVT_JOBS=4 tools/ci/core.sh dev
#   CC=gcc-14 CXX=g++-14 tools/ci/core.sh dev
#   CC=clang-18 CXX=clang++-18 CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ tools/ci/core.sh asan
#
# Environment: CVT_JOBS (default: number of CPUs), CVT_CMAKE_ARGS (extra configure
# arguments, e.g. "-G Ninja"), CVT_SKIP_TESTS=1 (build only).
set -euo pipefail

preset="${1:?usage: tools/ci/core.sh <dev|asan|release>}"
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

cpus() {
  getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}"
}
jobs="${CVT_JOBS:-$(cpus)}"

echo "==> configure (${preset})"
# shellcheck disable=SC2086 # CVT_CMAKE_ARGS is a list of words on purpose
cmake --preset "${preset}" ${CVT_CMAKE_ARGS:-}
echo "==> build (${preset}, -j ${jobs})"
cmake --build --preset "${preset}" -j "${jobs}"

if [[ "${CVT_SKIP_TESTS:-0}" == "1" || "${preset}" == "release" ]]; then
  exit 0
fi
echo "==> test (${preset})"
if [[ "${preset}" == "asan" && -n "${CVT_ASAN_EXTRA_OPTIONS:-}" ]]; then
  # A test preset's environment replaces the caller's, so extra sanitizer options need a
  # run without the preset: same build directory and configuration, the preset's
  # ASAN_OPTIONS/UBSAN_OPTIONS (read from CMakePresets.json) plus the extra ones.
  # Used for Ubuntu's shared libc++ 18, whose std::runtime_error is allocated with operator
  # new in libc++ and freed with free() in libc++abi (an alloc-dealloc-mismatch report that
  # is not a bug in Convertible): CVT_ASAN_EXTRA_OPTIONS=alloc_dealloc_mismatch=0.
  preset_option() { sed -n "s/.*\"$1\": *\"\\([^\"]*\\)\".*/\\1/p" CMakePresets.json | head -n 1; }
  ASAN_OPTIONS="$(preset_option ASAN_OPTIONS):${CVT_ASAN_EXTRA_OPTIONS}" \
  UBSAN_OPTIONS="$(preset_option UBSAN_OPTIONS)" \
    ctest --test-dir build/asan -C Debug --output-on-failure --timeout 3600
else
  ctest --preset "${preset}" --output-on-failure --timeout 3600
fi
