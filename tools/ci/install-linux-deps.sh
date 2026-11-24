#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Installs the Ubuntu 24.04 packages a CI job needs. Used by the workflows on GitHub's
# ubuntu-24.04 runners and by tools/ci/docker-linux.sh inside ubuntu:24.04 containers, so
# the two stay identical. Runs apt-get through sudo when not root.
#
#   tools/ci/install-linux-deps.sh gcc       GCC 14 + CMake + make (core job)
#   tools/ci/install-linux-deps.sh clang     Clang 18 + libc++ 18 (sanitizer job)
#   tools/ci/install-linux-deps.sh desktop   GCC 14 + X11 headers + Xvfb (desktop job)
#   tools/ci/install-linux-deps.sh hygiene   Python 3 + git (repository checks)
#
# Several sets may be given at once. Packages come from the distribution archive only.
set -euo pipefail

if [[ $# -eq 0 ]]; then
  sed -n '4,13p' "$0" >&2
  exit 2
fi

packages=()
for set in "$@"; do
  case "${set}" in
    gcc) packages+=(g++-14 gcc-14 cmake make) ;;
    clang) packages+=(clang-18 libc++-18-dev libc++abi-18-dev libclang-rt-18-dev llvm-18 cmake make) ;;
    desktop) packages+=(g++-14 gcc-14 cmake make libx11-dev libxext-dev xvfb x11-apps dpkg-dev file) ;;
    hygiene) packages+=(python3 git) ;;
    *) echo "install-linux-deps: unknown set '${set}'" >&2; exit 2 ;;
  esac
done

SUDO=""
if [[ "$(id -u)" != "0" ]]; then SUDO="sudo"; fi
export DEBIAN_FRONTEND=noninteractive
${SUDO} apt-get update -qq
${SUDO} apt-get install -y -qq --no-install-recommends "${packages[@]}" >/dev/null
echo "install-linux-deps: installed ${packages[*]}"
