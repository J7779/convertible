#!/usr/bin/env bash
# Builds the Convertible web app into apps/web/dist/ (a complete static site).
#
#   apps/web/build.sh            use a locally activated emsdk (emcc on PATH),
#                                otherwise the pinned emscripten/emsdk container
#   apps/web/build.sh --docker   always use the container
#   apps/web/build.sh --local    never use the container
#
# Environment: CVT_EMSDK_IMAGE (default below), CVT_JOBS (default 6),
#              CVT_DOCKER_MEMORY (default 4g).
set -euo pipefail

EMSDK_IMAGE="${CVT_EMSDK_IMAGE:-emscripten/emsdk:7.0.0}"
JOBS="${CVT_JOBS:-6}"

WEB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${WEB_DIR}/../.." && pwd)"
BUILD_DIR="${WEB_DIR}/build/wasm"
DIST_DIR="${WEB_DIR}/dist"

mode="auto"
case "${1:-}" in
  --docker) mode="docker" ;;
  --local) mode="local" ;;
  --inside-container) mode="inside" ;;
  "") ;;
  *) echo "usage: $0 [--docker|--local]" >&2; exit 2 ;;
esac

if [[ "${mode}" == "auto" ]]; then
  if command -v emcmake >/dev/null 2>&1; then mode="local"; else mode="docker"; fi
fi

if [[ "${mode}" == "docker" ]]; then
  command -v docker >/dev/null 2>&1 || { echo "error: neither emsdk (emcmake) nor docker is available" >&2; exit 1; }
  echo "==> building inside ${EMSDK_IMAGE}"
  # Only the repository is mounted. Files are written as the calling user.
  exec docker run --rm \
    --memory "${CVT_DOCKER_MEMORY:-4g}" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e CVT_JOBS="${JOBS}" \
    -v "${REPO_DIR}:/src" \
    -w /src \
    "${EMSDK_IMAGE}" \
    bash apps/web/build.sh --inside-container
fi

# ---------------------------------------------------------------------------
# Compile (local emsdk or inside the container)
# ---------------------------------------------------------------------------
command -v emcmake >/dev/null 2>&1 || { echo "error: emcmake not found; activate emsdk or use --docker" >&2; exit 1; }

# A writable, persistent Emscripten cache inside the build directory (system
# libraries are built on first use and reused afterwards; CI may cache it).
if [[ -z "${EM_CACHE:-}" ]]; then
  export EM_CACHE="${WEB_DIR}/build/emcache"
  if [[ ! -d "${EM_CACHE}/sysroot" ]]; then
    mkdir -p "${EM_CACHE}"
    default_cache="$(dirname "$(command -v emcc)")/cache"
    if [[ -d "${default_cache}/sysroot" ]]; then
      cp -R "${default_cache}/." "${EM_CACHE}/"
    fi
  fi
fi

NODE_BIN="${EMSDK_NODE:-node}"
command -v "${NODE_BIN}" >/dev/null 2>&1 || NODE_BIN="node"

echo "==> configuring ($(emcc --version | head -n 1))"
emcmake cmake -S "${WEB_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
echo "==> compiling the core to WebAssembly (-j ${JOBS})"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# ---------------------------------------------------------------------------
# Assemble the static site
# ---------------------------------------------------------------------------
echo "==> assembling ${DIST_DIR#"${REPO_DIR}/"}"
"${NODE_BIN}" "${WEB_DIR}/tools/assemble.mjs" \
  --repo "${REPO_DIR}" \
  --wasm-dir "${BUILD_DIR}" \
  --out "${DIST_DIR}"
