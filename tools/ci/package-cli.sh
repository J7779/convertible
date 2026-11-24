#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Packs the `convertible` command-line tool with LICENSE and NOTICES.md into a release
# archive. Uses `cmake -E tar`, so it works the same on Linux, macOS and Windows (Git Bash).
#
#   tools/ci/package-cli.sh <path to convertible[.exe]> <platform-label> <output dir>
#   tools/ci/package-cli.sh build/release/core/convertible linux-x64 release
#
# Output: <output dir>/convertible-cli-<version>-<platform-label>.tar.gz
#         (.zip when the label starts with "windows").
set -euo pipefail

cli="${1:?usage: package-cli.sh <cli> <platform-label> <outdir>}"
label="${2:?platform label, e.g. linux-x64}"
outdir="${3:?output directory}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$("${root}/tools/ci/version.sh")"

[[ -f "${cli}" ]] || { echo "package-cli: ${cli} not found" >&2; exit 1; }
mkdir -p "${outdir}"
outdir="$(cd "${outdir}" && { pwd -W 2>/dev/null || pwd; })"  # native path for cmake.exe in Git Bash
name="convertible-cli-${version}-${label}"
stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT
mkdir -p "${stage}/${name}"
cp "${cli}" "${stage}/${name}/"
cp "${root}/LICENSE" "${root}/NOTICES.md" "${stage}/${name}/"
cat > "${stage}/${name}/README.txt" <<EOF
Convertible ${version} - command-line tool

  convertible formats                 list every known format
  convertible detect <file>           name the format of a file
  convertible targets <file>          list what a file can be converted to
  convertible convert <file> -t <id>  convert (see: convertible --help)

Everything runs offline on this computer. LICENSE is the MIT licence of Convertible;
NOTICES.md lists the third-party components it contains and their licences.
EOF

if [[ "${label}" == windows* ]]; then
  archive="${outdir}/${name}.zip"
  (cd "${stage}" && cmake -E tar cf "${archive}" --format=zip "${name}")
else
  chmod 755 "${stage}/${name}/$(basename "${cli}")"
  archive="${outdir}/${name}.tar.gz"
  (cd "${stage}" && cmake -E tar czf "${archive}" "${name}")
fi
echo "package-cli: ${archive}"
