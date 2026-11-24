#!/bin/sh
# Builds the Convertible C++ core as a static-library XCFramework for iOS:
#   device     arm64            (iphoneos)
#   simulator  arm64 + x86_64   (iphonesimulator)
#
# Output: apps/ios/build/ConvertibleCore.xcframework (git-ignored).
# The Xcode project runs this script as a pre-build phase. It returns
# immediately when the XCFramework exists and no core source file changed.
#
# Usage: apps/ios/scripts/build-core.sh [--force]
# Environment: CVT_JOBS (parallel compile jobs, default 6).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
IOS_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(cd "$IOS_DIR/../.." && pwd)
CORE_DIR="$REPO_DIR/core"
BUILD_DIR="$IOS_DIR/build"
XCFRAMEWORK="$BUILD_DIR/ConvertibleCore.xcframework"
STAMP="$BUILD_DIR/core.stamp"
DEPLOYMENT_TARGET="16.4"
JOBS="${CVT_JOBS:-6}"

# Fingerprint of everything that influences the library: core sources,
# headers, CMake files and this script.
fingerprint() {
  (
    cd "$REPO_DIR"
    find core/CMakeLists.txt core/cmake core/include core/src -type f -print | LC_ALL=C sort | xargs shasum -a 256
    shasum -a 256 apps/ios/scripts/build-core.sh
  ) | shasum -a 256 | cut -d ' ' -f 1
}

CURRENT=$(fingerprint)
if [ "${1:-}" != "--force" ] && [ -d "$XCFRAMEWORK" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$CURRENT" ]; then
  echo "Convertible core is up to date."
  exit 0
fi

# When run from Xcode, the build environment (SDKROOT, ARCHS, PLATFORM_NAME,
# deployment-target variables ...) would leak into CMake's toolchain
# detection. Run CMake in a clean environment instead.
CLEAN_ENV="env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin HOME=${HOME:-/tmp}"
if [ -n "${DEVELOPER_DIR:-}" ]; then
  CLEAN_ENV="$CLEAN_ENV DEVELOPER_DIR=$DEVELOPER_DIR"
fi
CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ]; then
  for candidate in /opt/homebrew/bin/cmake /usr/local/bin/cmake /Applications/CMake.app/Contents/bin/cmake; do
    if [ -x "$candidate" ]; then CMAKE="$candidate"; break; fi
  done
fi
if [ -z "$CMAKE" ]; then
  echo "error: CMake 3.22 or newer is required to build the Convertible core (cmake not found in PATH)." >&2
  exit 1
fi

build_slice() {
  name="$1"; sysroot="$2"; archs="$3"
  dir="$BUILD_DIR/cmake-$name"
  echo "Building Convertible core for $name ($archs)..."
  $CLEAN_ENV "$CMAKE" -S "$CORE_DIR" -B "$dir" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sysroot" \
    -DCMAKE_OSX_ARCHITECTURES="$archs" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
    -DCVT_BUILD_TESTS=OFF \
    -DCVT_BUILD_CLI=OFF > "$dir.log" 2>&1 || { cat "$dir.log" >&2; exit 1; }
  $CLEAN_ENV "$CMAKE" --build "$dir" --config Release --parallel "$JOBS" >> "$dir.log" 2>&1 || { tail -n 60 "$dir.log" >&2; exit 1; }
}

mkdir -p "$BUILD_DIR"
build_slice iphoneos iphoneos "arm64"
build_slice iphonesimulator iphonesimulator "arm64;x86_64"

HEADERS="$BUILD_DIR/headers"
rm -rf "$HEADERS"
mkdir -p "$HEADERS"
cp "$CORE_DIR/include/convertible/convertible.h" "$HEADERS/"
cat > "$HEADERS/module.modulemap" <<'EOF'
module CConvertible {
  header "convertible.h"
  export *
}
EOF

rm -rf "$XCFRAMEWORK"
$CLEAN_ENV xcodebuild -create-xcframework \
  -library "$BUILD_DIR/cmake-iphoneos/libconvertible_core.a" -headers "$HEADERS" \
  -library "$BUILD_DIR/cmake-iphonesimulator/libconvertible_core.a" -headers "$HEADERS" \
  -output "$XCFRAMEWORK" > /dev/null

echo "$CURRENT" > "$STAMP"
echo "Built $XCFRAMEWORK"
