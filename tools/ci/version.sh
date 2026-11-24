#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Prints the project version from the root CMakeLists.txt (`project(Convertible VERSION x.y.z)`),
# the single source of truth that the core, the CLI and every app use.
#
#   tools/ci/version.sh                        # e.g. 0.1.0
#   tools/ci/version.sh --check-tag v0.1.0     # exit 1 unless the tag is v<version>
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

version="$(awk '/^project\(Convertible/{p=1} p && /VERSION/{for(i=1;i<=NF;i++) if($i=="VERSION"){print $(i+1); exit}}' CMakeLists.txt)"
[[ -n "${version}" ]] || { echo "version: no VERSION in CMakeLists.txt" >&2; exit 1; }

if [[ "${1:-}" == "--check-tag" ]]; then
  tag="${2:?usage: version.sh --check-tag <tag>}"
  if [[ "${tag}" != "v${version}" ]]; then
    echo "version: tag ${tag} does not match the project version ${version} (expected v${version});" \
         "bump VERSION in CMakeLists.txt first" >&2
    exit 1
  fi
fi
echo "${version}"
