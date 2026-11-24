#!/usr/bin/env bash
# End-to-end tests of the Android app on a headless emulator.
#
#   apps/android/e2e.sh            # the E2E suite (package app.convertible.e2e)
#   E2E_SCOPE=all apps/android/e2e.sh   # every instrumented test (E2E + core + UI)
#   E2E_RUNS=5 apps/android/e2e.sh      # repeat the run N times on the same emulator
#
# Creates the AVD if needed, boots it headless, waits for boot completion, runs
# `./gradlew connectedDebugAndroidTest`, copies reports into build/e2e/ and shuts the emulator down.
# Only the emulator started here is touched; other running emulators/devices are left alone
# (everything is addressed by serial).
#
# Environment (all optional except ANDROID_HOME):
#   ANDROID_HOME        Android SDK (required)
#   JAVA_HOME           JDK 17+ for Gradle (otherwise `java` on PATH)
#   E2E_API             system image API level            (default 37.0)
#   E2E_ABI             system image ABI                  (default: host arch -> arm64-v8a or x86_64)
#   E2E_TAG             system image tag                  (default: first installed of google_apis,
#                                                          google_apis_playstore, default)
#   E2E_AVD             AVD name                          (default convertible_e2e)
#   E2E_PORT            emulator console port (even)      (default 5570 -> serial emulator-5570)
#   E2E_RAM_MB          guest RAM                         (default 4096)
#   E2E_BOOT_TIMEOUT    seconds to wait for boot          (default 600)
#   E2E_SCOPE           e2e | all                         (default e2e)
#   E2E_CLASS           run one class or method, e.g. app.convertible.e2e.ConversionE2eTest#savesBase64
#   E2E_RUNS            number of consecutive runs        (default 1)
#   E2E_KEEP_EMULATOR   1 = leave the emulator running afterwards
#   E2E_BOOT_ONLY       1 = only create/boot/prepare the emulator and leave it running
#   E2E_EMULATOR_FLAGS  extra emulator flags
#   GRADLE_WORKERS      Gradle --max-workers              (default 6)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
export ANDROID_HOME
adb="$ANDROID_HOME/platform-tools/adb"
emulator_bin="$ANDROID_HOME/emulator/emulator"
[[ -x "$adb" ]] || { echo "missing platform-tools (sdkmanager 'platform-tools')" >&2; exit 2; }
[[ -x "$emulator_bin" ]] || { echo "missing emulator (sdkmanager 'emulator')" >&2; exit 2; }

api="${E2E_API:-37.0}"
case "$(uname -m)" in
  arm64|aarch64) default_abi=arm64-v8a ;;
  *) default_abi=x86_64 ;;
esac
abi="${E2E_ABI:-$default_abi}"
if [[ -n "${E2E_TAG:-}" ]]; then
  tag="$E2E_TAG"
else
  tag=""
  for t in google_apis google_apis_playstore default; do
    if [[ -d "$ANDROID_HOME/system-images/android-$api/$t/$abi" ]]; then tag="$t"; break; fi
  done
fi
image_rel="system-images/android-$api/${tag:-google_apis}/$abi/"
if [[ -z "$tag" || ! -f "$ANDROID_HOME/$image_rel/system.img" ]]; then
  echo "system image not installed: sdkmanager 'system-images;android-$api;${tag:-google_apis};$abi'" >&2
  exit 2
fi

avd="${E2E_AVD:-convertible_e2e}"
port="${E2E_PORT:-5570}"
serial="emulator-$port"
ram="${E2E_RAM_MB:-4096}"
boot_timeout="${E2E_BOOT_TIMEOUT:-600}"
scope="${E2E_SCOPE:-e2e}"
runs="${E2E_RUNS:-1}"
workers="${GRADLE_WORKERS:-6}"
avd_home="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
out="$here/build/e2e"
mkdir -p "$out" "$avd_home"

log() { printf '[e2e] %s\n' "$*"; }

# ------------------------------------------------------------------------------------------- AVD
create_avd() {
  local dir="$avd_home/$avd.avd"
  log "creating AVD $avd ($image_rel)"
  mkdir -p "$dir"
  cat > "$avd_home/$avd.ini" <<EOF
avd.ini.encoding=UTF-8
path=$dir
path.rel=avd/$avd.avd
target=android-$api
EOF
  local cpu_arch=arm64
  [[ "$abi" == x86_64 ]] && cpu_arch=x86_64
  cat > "$dir/config.ini" <<EOF
AvdId=$avd
avd.ini.displayname=$avd
avd.ini.encoding=UTF-8
abi.type=$abi
hw.cpu.arch=$cpu_arch
hw.cpu.ncore=4
hw.ramSize=$ram
vm.heapSize=512
disk.dataPartition.size=8G
hw.lcd.width=1080
hw.lcd.height=2400
hw.lcd.density=420
hw.keyboard=yes
hw.mainKeys=no
hw.gpu.enabled=yes
hw.gpu.mode=swiftshader_indirect
hw.sdCard=no
hw.audioInput=no
hw.camera.back=none
hw.camera.front=none
hw.initialOrientation=portrait
hw.accelerometer=yes
hw.sensors.orientation=yes
image.sysdir.1=$image_rel
tag.id=$tag
target=android-$api
showDeviceFrame=no
fastboot.forceColdBoot=yes
EOF
}
[[ -f "$avd_home/$avd.avd/config.ini" ]] || create_avd

# ------------------------------------------------------------------------------------------- boot
if "$adb" devices | grep -q "^$serial[[:space:]]*device"; then
  echo "$serial is already in use; set E2E_PORT to a free even port" >&2
  exit 2
fi

emu_pid=""
shutdown_emulator() {
  if [[ -n "$emu_pid" && "${E2E_KEEP_EMULATOR:-0}" != 1 ]]; then
    log "shutting down $serial"
    "$adb" -s "$serial" emu kill >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do kill -0 "$emu_pid" 2>/dev/null || break; sleep 1; done
    kill "$emu_pid" 2>/dev/null || true
  fi
}
trap shutdown_emulator EXIT

log "booting $avd on $serial (headless)"
# shellcheck disable=SC2086
ANDROID_AVD_HOME="$avd_home" "$emulator_bin" -avd "$avd" -port "$port" \
  -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect \
  -no-snapshot -wipe-data -camera-back none -camera-front none ${E2E_EMULATOR_FLAGS:-} \
  > "$out/emulator.log" 2>&1 &
emu_pid=$!

deadline=$((SECONDS + boot_timeout))
until [[ "$("$adb" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]]; do
  if ! kill -0 "$emu_pid" 2>/dev/null; then echo "emulator exited; see $out/emulator.log" >&2; exit 1; fi
  if (( SECONDS > deadline )); then echo "boot timed out after ${boot_timeout}s" >&2; exit 1; fi
  sleep 3
done
until "$adb" -s "$serial" shell pm path android >/dev/null 2>&1; do sleep 2; done
log "booted in ${SECONDS}s"

sh_dev() { "$adb" -s "$serial" shell "$@"; }
sh_dev settings put global window_animation_scale 0
sh_dev settings put global transition_animation_scale 0
sh_dev settings put global animator_duration_scale 1
sh_dev settings put system font_scale 1.0
sh_dev settings put secure show_ime_with_hard_keyboard 0
sh_dev svc power stayon true
sh_dev wm dismiss-keyguard >/dev/null 2>&1 || true
sh_dev input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true

if [[ "${E2E_BOOT_ONLY:-0}" == 1 ]]; then
  log "emulator left running as $serial (stop it with: adb -s $serial emu kill)"
  emu_pid=""
  exit 0
fi

# ------------------------------------------------------------------------------------------- run
gradle_args=(--max-workers="$workers" "-Pconvertible.abis=$abi" connectedDebugAndroidTest)
if [[ -n "${E2E_CLASS:-}" ]]; then
  gradle_args+=("-Pandroid.testInstrumentationRunnerArguments.class=$E2E_CLASS")
elif [[ "$scope" == e2e ]]; then
  gradle_args+=("-Pandroid.testInstrumentationRunnerArguments.package=app.convertible.e2e")
fi

status=0
summary="$out/summary.txt"
: > "$summary"
for run in $(seq 1 "$runs"); do
  log "run $run/$runs: ./gradlew ${gradle_args[*]}"
  start=$SECONDS
  if ANDROID_SERIAL="$serial" ./gradlew "${gradle_args[@]}"; then result=PASS; else result=FAIL; status=1; fi
  took=$((SECONDS - start))
  dest="$out/run-$run"
  rm -rf "$dest"; mkdir -p "$dest"
  cp -R app/build/reports/androidTests/connected "$dest/report" 2>/dev/null || true
  cp -R app/build/outputs/androidTest-results/connected "$dest/results" 2>/dev/null || true
  # Failure screenshots written through the test storage service.
  cp -R app/build/outputs/connected_android_test_additional_output "$dest/screenshots" 2>/dev/null || true
  counts="$(python3 - "$dest/results" <<'PY' 2>/dev/null || true
import sys, pathlib, xml.etree.ElementTree as ET
t = f = e = s = 0
for p in pathlib.Path(sys.argv[1]).rglob("TEST-*.xml"):
    r = ET.parse(p).getroot()
    t += int(r.get("tests", 0)); f += int(r.get("failures", 0)); e += int(r.get("errors", 0)); s += int(r.get("skipped", 0))
print(f"tests={t} failures={f} errors={e} skipped={s}")
PY
)"
  line="run $run: $result in ${took}s ${counts}"
  log "$line"
  echo "$line" >> "$summary"
done

log "reports in build/e2e/ (summary.txt, run-N/report/index.html)"
cat "$summary"
exit $status
