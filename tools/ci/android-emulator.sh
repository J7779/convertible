#!/usr/bin/env bash
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#
# Headless Android emulator for CI and local runs, using only the SDK's own tools
# (sdkmanager, avdmanager, emulator, adb) - no third-party actions or scripts.
#
#   tools/ci/android-emulator.sh install    install emulator, platform-tools and the system image
#   tools/ci/android-emulator.sh create     create the AVD (idempotent)
#   tools/ci/android-emulator.sh boot       start it in the background, without a window
#   tools/ci/android-emulator.sh wait       block until Android has fully booted, then unlock
#   tools/ci/android-emulator.sh shutdown   stop it
#   tools/ci/android-emulator.sh up         install + create + boot + wait
#
# Environment:
#   ANDROID_HOME (or ANDROID_SDK_ROOT)  the SDK (preinstalled on GitHub's ubuntu runners)
#   CVT_AVD_API      system image API level   (default 37.0, see apps/android/README.md)
#   CVT_AVD_TAG      system image tag         (default google_apis)
#   CVT_AVD_ABI      system image ABI         (default x86_64; arm64-v8a on Apple silicon)
#   CVT_AVD_NAME     AVD name                 (default convertible-ci)
#   CVT_BOOT_TIMEOUT seconds to wait for boot (default 600)
# Hardware acceleration: Linux needs /dev/kvm to be accessible (the CI workflow grants it
# with a udev rule); without it the emulator is far too slow to be useful.
set -euo pipefail

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[[ -n "${SDK}" && -d "${SDK}" ]] || { echo "android-emulator: set ANDROID_HOME" >&2; exit 2; }
API="${CVT_AVD_API:-37.0}"
TAG="${CVT_AVD_TAG:-google_apis}"
ABI="${CVT_AVD_ABI:-x86_64}"
NAME="${CVT_AVD_NAME:-convertible-ci}"
TIMEOUT="${CVT_BOOT_TIMEOUT:-600}"
IMAGE="system-images;android-${API};${TAG};${ABI}"

find_tool() {
  local name="$1"; shift
  local candidate
  for candidate in "$@"; do
    if [[ -x "${candidate}" ]]; then echo "${candidate}"; return; fi
  done
  command -v "${name}" || { echo "android-emulator: ${name} not found in the SDK" >&2; exit 2; }
}
SDKMANAGER="$(find_tool sdkmanager "${SDK}/cmdline-tools/latest/bin/sdkmanager")"
AVDMANAGER="$(find_tool avdmanager "${SDK}/cmdline-tools/latest/bin/avdmanager")"
EMULATOR="${SDK}/emulator/emulator"
ADB="${SDK}/platform-tools/adb"

install() {
  yes | "${SDKMANAGER}" --licenses >/dev/null || true
  "${SDKMANAGER}" --install "emulator" "platform-tools" "${IMAGE}" >/dev/null
  echo "android-emulator: installed ${IMAGE}"
}

create() {
  if "${EMULATOR}" -list-avds 2>/dev/null | grep -qx "${NAME}"; then
    echo "android-emulator: AVD ${NAME} exists"
    return
  fi
  echo "no" | "${AVDMANAGER}" create avd --force --name "${NAME}" --package "${IMAGE}" --device "pixel_6" >/dev/null
  # Enough memory and storage for the instrumented tests (a 64 MiB file through the path API).
  local config="${ANDROID_AVD_HOME:-${HOME}/.android/avd}/${NAME}.avd/config.ini"
  if [[ -f "${config}" ]]; then
    printf 'hw.ramSize=4096\ndisk.dataPartition.size=6G\nhw.keyboard=yes\n' >> "${config}"
  fi
  echo "android-emulator: created ${NAME}"
}

boot() {
  local log="${CVT_EMULATOR_LOG:-${TMPDIR:-/tmp}/convertible-emulator.log}"
  nohup "${EMULATOR}" -avd "${NAME}" -no-window -no-audio -no-boot-anim -no-snapshot \
    -gpu swiftshader_indirect -camera-back none -camera-front none -no-metrics \
    -accel auto > "${log}" 2>&1 &
  echo "android-emulator: booting ${NAME} (pid $!, log ${log})"
}

wait_booted() {
  "${ADB}" start-server >/dev/null
  local deadline=$(( $(date +%s) + TIMEOUT ))
  "${ADB}" wait-for-device
  until [[ "$("${ADB}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == "1" ]]; do
    if (( $(date +%s) > deadline )); then
      echo "android-emulator: not booted after ${TIMEOUT}s" >&2
      exit 1
    fi
    sleep 5
  done
  # Quiet, deterministic device for UI tests.
  "${ADB}" shell input keyevent 82 >/dev/null 2>&1 || true
  for setting in window_animation_scale transition_animation_scale animator_duration_scale; do
    "${ADB}" shell settings put global "${setting}" 0 >/dev/null 2>&1 || true
  done
  echo "android-emulator: booted ($("${ADB}" shell getprop ro.build.version.release | tr -d '\r'))"
}

shutdown() {
  "${ADB}" emu kill >/dev/null 2>&1 || true
  echo "android-emulator: stopped"
}

case "${1:-}" in
  install) install ;;
  create) create ;;
  boot) boot ;;
  wait) wait_booted ;;
  shutdown) shutdown ;;
  up) install; create; boot; wait_booted ;;
  *) sed -n '4,22p' "$0" >&2; exit 2 ;;
esac
