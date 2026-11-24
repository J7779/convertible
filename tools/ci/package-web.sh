#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Zips the built web app (apps/web/dist, from apps/web/build.sh) for a release:
#
#   tools/ci/package-web.sh <output dir>   ->  <output dir>/convertible-web-<version>.zip
#
# The archive is a complete static site; serve its contents from any HTTPS host (see
# apps/web/README.md, "Hosting checklist").
set -euo pipefail

outdir="${1:?usage: package-web.sh <outdir>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$("${root}/tools/ci/version.sh")"
dist="${root}/apps/web/dist"
[[ -f "${dist}/index.html" ]] || { echo "package-web: no build in apps/web/dist" >&2; exit 1; }

mkdir -p "${outdir}"
outdir="$(cd "${outdir}" && { pwd -W 2>/dev/null || pwd; })"  # native path for cmake.exe in Git Bash
archive="${outdir}/convertible-web-${version}.zip"
rm -f "${archive}"
(cd "${dist}" && cmake -E tar cf "${archive}" --format=zip .)
echo "package-web: ${archive}"
