#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Rehearses the Linux CI jobs locally inside throwaway containers, so nothing is installed
# on the host. The repository is mounted read-only and copied into the container (build
# directories excluded), so host builds in build/ are never touched or reused.
#
#   tools/ci/docker-linux.sh gcc        core, GCC 14, dev preset (+ formats doc check)
#   tools/ci/docker-linux.sh clang      core, Clang 18 + libc++ 18, asan preset (ASan + UBSan)
#   tools/ci/docker-linux.sh desktop    desktop app: build, desktop tests (+ e2e label), package
#   tools/ci/docker-linux.sh hygiene    privacy audit (+ self-test), skills mirror, notices, workflow parse
#   tools/ci/docker-linux.sh mingw      MinGW-w64 cross build of core + CLI (smoke check)
#   tools/ci/docker-linux.sh shell      interactive shell in the gcc environment
#
# Environment: CVT_DOCKER_MEMORY (default 6g), CVT_JOBS (default 6),
#              CVT_DOCKER_IMAGE (default ubuntu:24.04; mingw uses debian:trixie, whose
#              MinGW is GCC 14 - Ubuntu 24.04's MinGW is GCC 13 and cannot build the core).
#
# Differences from the GitHub runner: a minimal image instead of the full runner image (the
# needed packages are installed by tools/ci/install-linux-deps.sh, the same script CI uses),
# the distribution's CMake 3.28 instead of the runner's newer CMake, no caches, and memory
# capped by --memory. Results land in the container only; logs are printed.
set -euo pipefail

job="${1:-}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MEMORY="${CVT_DOCKER_MEMORY:-6g}"
JOBS="${CVT_JOBS:-6}"
IMAGE="${CVT_DOCKER_IMAGE:-ubuntu:24.04}"

# Copy exactly the files git would commit (tracked plus untracked-but-not-ignored, so no
# build output from host builds) to /work/src, then run the job there.
FILE_LIST="$(mktemp)"
trap 'rm -f "${FILE_LIST}"' EXIT
git -C "${REPO}" ls-files -z --cached --others --exclude-standard > "${FILE_LIST}"
prologue='set -euo pipefail
mkdir -p /work/src
tar -C /src --null --ignore-failed-read -T /work-files -cf - 2>/dev/null | tar -C /work/src -xf -
cd /work/src
'

case "${job}" in
  gcc)
    script='tools/ci/install-linux-deps.sh gcc hygiene
CC=gcc-14 CXX=g++-14 CVT_JOBS='"${JOBS}"' tools/ci/core.sh dev
python3 tools/gen-formats-doc --check --cli build/dev/core/convertible' ;;
  clang)
    script='tools/ci/install-linux-deps.sh clang
export ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer
export CVT_ASAN_EXTRA_OPTIONS=alloc_dealloc_mismatch=0
CC=clang-18 CXX=clang++-18 CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ CVT_JOBS='"${JOBS}"' tools/ci/core.sh asan' ;;
  desktop)
    script='tools/ci/install-linux-deps.sh desktop
CC=gcc-14 CXX=g++-14 CVT_JOBS='"${JOBS}"' tools/ci/desktop.sh' ;;
  hygiene)
    script='tools/ci/install-linux-deps.sh hygiene
apt-get install -y -qq --no-install-recommends python3-yaml >/dev/null
git init -q . && git add -A >/dev/null && git -c user.name=ci -c user.email=ci@users.noreply.github.com commit -qm snapshot
python3 tools/privacy-audit --self-test
python3 tools/privacy-audit
python3 tools/sync-skills --check
python3 tools/gen-notices --check
python3 - <<PY
import glob, yaml
for path in sorted(glob.glob(".github/workflows/*.yml") + glob.glob(".github/actions/*/action.yml")):
    yaml.safe_load(open(path, encoding="utf-8"))
    print("yaml ok:", path)
PY' ;;
  mingw)
    IMAGE="${CVT_DOCKER_IMAGE:-debian:trixie}"
    script='export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq --no-install-recommends g++-mingw-w64-x86-64-posix cmake make >/dev/null
x86_64-w64-mingw32-g++-posix --version | head -n 1
cmake -S . -B build/mingw -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
  -DCVT_BUILD_TESTS=OFF -DCVT_BUILD_CLI=ON
cmake --build build/mingw -j '"${JOBS}"'
ls -l build/mingw/core/convertible.exe' ;;
  shell)
    docker run --rm -it --memory "${MEMORY}" -v "${REPO}:/src:ro" -v "${FILE_LIST}:/work-files:ro" \
      "${IMAGE}" bash -c "${prologue} tools/ci/install-linux-deps.sh gcc; exec bash"
    exit $? ;;
  *)
    sed -n '4,20p' "$0" >&2
    exit 2 ;;
esac

echo "==> ${job} in ${IMAGE} (memory ${MEMORY}, -j ${JOBS})"
start=$(date +%s)
# Nothing from the host environment is passed in: every job builds with the flags in the
# CMake files only, exactly like CI.
docker run --rm --memory "${MEMORY}" \
  -v "${REPO}:/src:ro" -v "${FILE_LIST}:/work-files:ro" "${IMAGE}" bash -c "${prologue}${script}"
echo "==> ${job}: passed in $(( $(date +%s) - start ))s"
