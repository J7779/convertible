#!/usr/bin/env bash
# End-to-end (XCUITest) suite for the iOS/iPadOS app, on a named simulator.
#
#   apps/ios/e2e.sh                                  # iPhone 18 Pro, whole suite once
#   apps/ios/e2e.sh --device "iPad Pro 11-inch (M5)"
#   apps/ios/e2e.sh --repeat 5 --clone               # five runs on a private copy of the device
#   apps/ios/e2e.sh --only ConversionTests           # one class (or Class/testMethod)
#
# Builds once (build-for-testing), then runs `xcodebuild test-without-building
# -only-testing:ConvertibleUITests CODE_SIGNING_ALLOWED=NO`. Result bundles go to
# apps/ios/build/e2e/ (git-ignored). Local only: there is no iOS job in CI.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
device="iPhone 18 Pro"
repeat=1
clone=0
only="ConvertibleUITests"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) device="$2"; shift 2 ;;
    --repeat) repeat="$2"; shift 2 ;;
    --clone) clone=1; shift ;;
    --only) only="ConvertibleUITests/$2"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

out="$here/build/e2e"
derived="$here/build/DerivedData"
mkdir -p "$out"

# The first available simulator with this exact name.
udid="$(xcrun simctl list devices available -j | /usr/bin/python3 -I -c '
import json, sys
name = sys.argv[1]
for runtime, devices in json.load(sys.stdin)["devices"].items():
    for d in devices:
        if d["name"] == name and d.get("isAvailable", True):
            print(d["udid"]); sys.exit(0)
sys.exit(1)' "$device")" || { echo "no available simulator named \"$device\"" >&2; exit 1; }

booted_here=0
cloned=""
seeded=""
cleanup() {
  [[ -n "$seeded" ]] && rm -f "$seeded"
  if [[ -n "$cloned" ]]; then
    xcrun simctl shutdown "$cloned" >/dev/null 2>&1 || true
    xcrun simctl delete "$cloned" >/dev/null 2>&1 || true
  elif [[ $booted_here == 1 ]]; then
    xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if [[ $clone == 1 ]]; then
  # A private copy, so another run on the same simulator cannot collide with this one.
  xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
  cloned="$(xcrun simctl clone "$udid" "Convertible E2E $$ ($device)")"
  udid="$cloned"
fi

if ! xcrun simctl list devices | grep -q "$udid) (Booted)"; then
  xcrun simctl boot "$udid"
  booted_here=1
fi
xcrun simctl bootstatus "$udid" -b >/dev/null

# Seed one text file into the device's "On My iPhone/iPad" storage for the document-picker test.
storage="$(xcrun simctl get_app_container "$udid" com.apple.DocumentsApp groups 2>/dev/null \
  | awk '/group.com.apple.FileProvider.LocalStorage/ { $1 = ""; sub(/^ /, ""); print; exit }')" || true
if [[ -n "$storage" && -d "$storage" ]]; then
  mkdir -p "$storage/File Provider Storage"
  seeded_name="E2E picked file.txt"
  seeded_text="Picked through the system document picker."
  seeded="$storage/File Provider Storage/$seeded_name"
  printf '%s\n' "$seeded_text" > "$seeded"
  export TEST_RUNNER_E2E_SEEDED_FILE="$seeded_name"
  export TEST_RUNNER_E2E_SEEDED_TEXT="$seeded_text"$'\n'
fi

common=(-project "$here/Convertible.xcodeproj" -scheme Convertible
        -destination "platform=iOS Simulator,id=$udid" -derivedDataPath "$derived"
        CODE_SIGNING_ALLOWED=NO)

echo "Building for testing on $device…"
xcodebuild build-for-testing "${common[@]}" -quiet

status=0
passed=0
for ((run = 1; run <= repeat; run++)); do
  stamp="$(date +%Y%m%d-%H%M%S)"
  bundle="$out/${device// /-}-$stamp-run$run.xcresult"
  start=$SECONDS
  set +e
  xcodebuild test-without-building "${common[@]}" -only-testing:"$only" -resultBundlePath "$bundle" 2>&1 \
    | tee "$out/last-run.log" | grep -E "^Test Case .*(passed|failed|skipped)|^Executed|error:"
  result=${PIPESTATUS[0]}
  set -e
  echo "Run $run/$repeat on $device: $([[ $result == 0 ]] && echo PASSED || echo FAILED) in $((SECONDS - start)) s — $bundle"
  if [[ $result == 0 ]]; then passed=$((passed + 1)); else status=1; fi
done
echo "$passed of $repeat runs passed on $device"
exit $status
