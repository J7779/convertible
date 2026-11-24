#!/bin/sh
# End-to-end tests of the desktop app (macOS, Linux): configure, build, run.
#
#   apps/desktop/e2e.sh [--windowed] [--repeat N] [--build-dir DIR] [-- extra ctest arguments]
#
#   --windowed   also drive the real OS window (Cocoa; X11, through xvfb-run
#                when there is no display and xvfb-run is installed)
#   --repeat N   run the suite N times in a row (stops at the first failure)
#
# Same as, after the normal desktop build:
#   ctest --test-dir build/desktop -L desktop-e2e -C Release --output-on-failure
# Scratch folders and failure screenshots: <build>/apps/desktop/e2e/{runs,failures}.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build="$root/build/desktop"
windowed=0
repeat=1
while [ $# -gt 0 ]; do
  case "$1" in
    --windowed) windowed=1 ;;
    --repeat) repeat="$2"; shift ;;
    --build-dir) build="$2"; shift ;;
    --) shift; break ;;
    *) break ;;
  esac
  shift
done

cmake -S "$root" -B "$build" -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j 6 --config Release --target convertible_desktop convertible_desktop_e2e

runner=""
if [ "$windowed" = 1 ]; then
  CVD_E2E_WINDOWED=1
  export CVD_E2E_WINDOWED
  if [ "$(uname -s)" = Linux ] && [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
    runner="xvfb-run -a"
  fi
fi

i=1
while [ "$i" -le "$repeat" ]; do
  echo "== desktop end-to-end run $i of $repeat"
  $runner ctest --test-dir "$build" -L desktop-e2e -C Release --output-on-failure "$@"
  i=$((i + 1))
done
