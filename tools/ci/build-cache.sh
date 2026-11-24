#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Makes a build directory restored by actions/cache safe to build on incrementally,
# without a compiler cache.
#
# The problem: a fresh checkout gives every source file the current time, so make,
# Ninja and MSBuild would consider every object stale and rebuild everything, and simply
# back-dating all sources would be wrong whenever a file changed since the cache was
# saved. Instead, `save` records the git blob id of every tracked file next to the
# objects, and `restore` back-dates exactly the files whose content is unchanged since
# then. Changed, added or removed files keep their checkout time, so the build tool
# rebuilds them and (through its own dependency tracking) everything that includes them.
#
#   tools/ci/build-cache.sh restore <build-dir>   after actions/cache restored the dir
#   tools/ci/build-cache.sh save <build-dir>      after a successful build
#
# A missing or empty build directory, or one without a manifest, is left alone (a full
# build follows). Works with GNU and BSD touch and in Git Bash on Windows.
set -euo pipefail

mode="${1:?usage: tools/ci/build-cache.sh <restore|save> <build-dir>}"
dir="${2:?usage: tools/ci/build-cache.sh <restore|save> <build-dir>}"
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
manifest="${dir}/.cvt-source-manifest"
OLD="200001010000" # any time before the objects in the cache were built

case "${mode}" in
  save)
    [[ -d "${dir}" ]] || { echo "build-cache: ${dir} does not exist; nothing to record"; exit 0; }
    git ls-files -s > "${manifest}"
    echo "build-cache: recorded $(( $(wc -l < "${manifest}") )) source files in ${manifest}"
    ;;
  restore)
    if [[ ! -f "${manifest}" ]]; then
      echo "build-cache: no manifest in ${dir}; full build"
      exit 0
    fi
    current="$(mktemp)"
    unchanged="$(mktemp)"
    trap 'rm -f "${current}" "${unchanged}"' EXIT
    git ls-files -s > "${current}"
    # Lines are "<mode> <blob> <stage>\t<path>": identical lines = identical content.
    LC_ALL=C comm -12 <(LC_ALL=C sort "${manifest}") <(LC_ALL=C sort "${current}") | cut -f2- > "${unchanged}"
    total=$(( $(wc -l < "${current}") ))
    same=$(( $(wc -l < "${unchanged}") ))
    if [[ "${same}" -gt 0 ]]; then
      tr '\n' '\0' < "${unchanged}" | xargs -0 touch -c -t "${OLD}"
    fi
    echo "build-cache: ${same} of ${total} files unchanged since the cache was saved; $((total - same)) rebuild"
    ;;
  *)
    echo "usage: tools/ci/build-cache.sh <restore|save> <build-dir>" >&2
    exit 2
    ;;
esac
