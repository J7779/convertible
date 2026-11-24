#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Copies the APKs built by `./gradlew assembleDebug assembleRelease` (apps/android) into a
# release directory with versioned names:
#
#   convertible-android-<version>-<abi|universal>-release-unsigned.apk
#   convertible-android-<version>-<abi|universal>-debug.apk      (signed with a debug key)
#
#   tools/ci/package-android.sh <output dir>
#
# Release APKs are unsigned on purpose: the owner signs them with the release key outside
# CI (apksigner sign --ks <keystore> <apk>). Debug APKs install directly for testing.
set -euo pipefail

outdir="${1:?usage: package-android.sh <outdir>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$("${root}/tools/ci/version.sh")"
apks="${root}/apps/android/app/build/outputs/apk"
mkdir -p "${outdir}"

count=0
for apk in "${apks}"/debug/*.apk "${apks}"/release/*.apk; do
  [[ -f "${apk}" ]] || continue
  base="$(basename "${apk}" .apk)"          # app-arm64-v8a-release-unsigned
  rest="${base#app-}"                        # arm64-v8a-release-unsigned
  cp "${apk}" "${outdir}/convertible-android-${version}-${rest}.apk"
  count=$((count + 1))
done
[[ "${count}" -gt 0 ]] || { echo "package-android: no APKs under ${apks}" >&2; exit 1; }
echo "package-android: ${count} APKs in ${outdir}"
