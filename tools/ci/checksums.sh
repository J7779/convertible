#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Writes SHA256SUMS.txt for every file in a release directory (sorted by name, in the
# format `sha256sum -c` and `shasum -a 256 -c` accept).
#
#   tools/ci/checksums.sh <release dir>
#   (cd <release dir> && sha256sum -c SHA256SUMS.txt)     # verify
set -euo pipefail

dir="${1:?usage: checksums.sh <release dir>}"
cd "${dir}"
if command -v sha256sum >/dev/null 2>&1; then
  hash() { sha256sum "$@"; }
else
  hash() { shasum -a 256 "$@"; }
fi

files=()
while IFS= read -r name; do files+=("${name}"); done < <(find . -maxdepth 1 -type f ! -name SHA256SUMS.txt -exec basename {} \; | LC_ALL=C sort)
[[ "${#files[@]}" -gt 0 ]] || { echo "checksums: ${dir} is empty" >&2; exit 1; }
hash "${files[@]}" > SHA256SUMS.txt
echo "checksums: ${#files[@]} files in ${dir}/SHA256SUMS.txt"
