#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build, flash and probe the RA8D2 + OV5640 + ESP32-C6 browser livestream.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/hil/lib/rig_env.sh
# shellcheck disable=SC1091
source "$ROOT/scripts/hil/lib/rig_env.sh"
LOCAL_PI=0
if rig_is_local_pi; then
  LOCAL_PI=1
else
  rig_require PI_HOST
  : "${PI_HOST:?}"
fi
# shellcheck source=scripts/hil/lib/bench_lock.sh
# shellcheck disable=SC1091
source "$ROOT/scripts/hil/lib/bench_lock.sh"

APP="$ROOT/examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream"
BUILD_DIR_OWNED=0
if [[ -n "${C6_CAMERA_BUILD_DIR:-}" ]]; then
  BUILD_DIR="$C6_CAMERA_BUILD_DIR"
else
  BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ra8-c6-camera-build.XXXXXX")"
  BUILD_DIR_OWNED=1
fi
WIFI_ENV="$ROOT/coprocessor/esp32c6/wifi.env"
LOG="$(mktemp "${TMPDIR:-/tmp}/c6-camera-uart.XXXXXX")"
ARTIFACT_DIR="${C6_CAMERA_ARTIFACT_DIR:-/tmp/ra8-camera-livestream}"
REMOTE_PREFIX="/tmp/c6-camera-frame-$$"
REMOTE_FILES_CREATED=0
DECODE_DIR=""
run_on_pi() {
  if [[ "$LOCAL_PI" == 1 ]]; then
    bash -lc "$1"
  else
    # shellcheck disable=SC2029  # forwarding this caller-composed command verbatim is the helper's purpose.
    ssh "$PI_HOST" "$1"
  fi
}
copy_from_pi() {
  local source="$1"
  local destination="$2"
  if [[ "$LOCAL_PI" == 1 ]]; then
    cp "$source" "$destination"
  else
    scp -q "${PI_HOST}:${source}" "$destination"
  fi
}
cleanup_local() {
  rm -f "$LOG"
  if [[ -n "$DECODE_DIR" ]]; then
    rm -rf "$DECODE_DIR"
  fi
  if [[ "$BUILD_DIR_OWNED" == 1 ]]; then
    rm -rf "$BUILD_DIR"
  fi
  return 0
}
cleanup_remote() {
  if [[ "$REMOTE_FILES_CREATED" != 1 ]]; then
    return 0
  fi
  if run_on_pi "rm -f '${REMOTE_PREFIX}-1.jpg' '${REMOTE_PREFIX}-2.jpg'"; then
    REMOTE_FILES_CREATED=0
  fi
  return 0
}
trap cleanup_local EXIT

if [[ -f "$WIFI_ENV" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$WIFI_ENV"
  set +a
fi
if [[ -z "${RA8_C6_WIFI_PSK:-}" ]]; then
  RA8_C6_WIFI_PSK="$(python3 "$ROOT/scripts/secrets/openbao_client.py" \
    get secret/ra8d2/bench-network bench_psk)"
  RA8_C6_WIFI_SSID="${RA8_C6_WIFI_SSID:-ra8-bench}"
fi
: "${RA8_C6_WIFI_SSID:?missing RA8_C6_WIFI_SSID}"
: "${RA8_C6_WIFI_PSK:?missing RA8_C6_WIFI_PSK}"

RA8_C6_WIFI_SSID="$RA8_C6_WIFI_SSID" RA8_C6_WIFI_PSK="$RA8_C6_WIFI_PSK" \
  cmake -S "$APP" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-ra8d2.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRA8_USE_THREADX=ON -DRA8_USE_NETXDUO=ON -DRA8_USE_ESP_HOSTED=ON
unset RA8_C6_WIFI_SSID RA8_C6_WIFI_PSK
cmake --build "$BUILD_DIR" --parallel 4
ra8_bench_require "validate C6 camera livestream" 30m || exit $?
# Run remote cleanup before the guard's EXIT handler releases the bench.
_ra8_bench_add_exit_trap cleanup_remote

echo "camera_livestream: physical precondition: SW4 1=OFF 2=OFF 3=ON 4=OFF; C6 on J26" >&2
c6_ready=false
for attempt in 1 2 3; do
  echo "camera_livestream: C6 cold-start attempt $attempt/3" >&2
  bash "$ROOT/scripts/hil/tapo.sh" board cycle
  run_on_pi "bash -lc 'i=0
    while [ \"\$i\" -lt 60 ]; do
      if compgen -G \"/dev/serial/by-id/usb-SEGGER_J-Link_*-if00\" >/dev/null &&
         compgen -G \"/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00\" >/dev/null &&
         compgen -G \"/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00\" >/dev/null; then
        exit 0
      fi
      i=\$((i + 1))
      sleep 1
    done
    exit 1'"
  sleep 2
  if make -C "$ROOT" hil-c6 APP=c6_spi_probe; then
    c6_ready=true
    break
  fi
done
if [[ "$c6_ready" != true ]]; then
  echo "camera_livestream: C6 failed its SPI readiness probe after 3 cold starts" >&2
  exit 1
fi
bash "$ROOT/scripts/hil/run_direct.sh" \
  --hex "$BUILD_DIR/c6_camera_livestream.hex" \
  --expect 'c6_cam: PASS HTTP camera server listening' \
  --expect-negative 'c6_cam: FAIL|HardFault' --timeout 90 | tee "$LOG"

BOARD_IP="$(sed -n 's/.*c6_cam: PASS Wi-Fi and DHCP ip=\([0-9.]*\).*/\1/p' "$LOG" | tail -1)"
[[ "$BOARD_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "camera_livestream: could not parse board IP from UART" >&2
  exit 1
}

REMOTE_FILES_CREATED=1
run_on_pi "set -euo pipefail
  health=\$(curl -fsS --max-time 10 'http://${BOARD_IP}/health')
  test \"\$health\" = 'PASS c6 camera livestream'
  for n in 1 2; do
    f=${REMOTE_PREFIX}-\${n}.jpg
    curl -fsS --max-time 20 'http://${BOARD_IP}/frame.jpg?probe='\$n -o \"\$f\"
    test \"\$(od -An -tx1 -N2 \"\$f\" | tr -d ' ')\" = ffd8
    test \"\$(stat -c %s \"\$f\")\" -gt 1000
    sleep 1
  done"

mkdir -p "$ARTIFACT_DIR"
for n in 1 2; do
  copy_from_pi "${REMOTE_PREFIX}-${n}.jpg" "$ARTIFACT_DIR/frame-${n}.jpg"
done
cleanup_remote

validate_jpeg() {
  local frame="$1" dimensions width height
  if command -v sips >/dev/null 2>&1; then
    width="$(sips -g pixelWidth "$frame" 2>/dev/null | awk '/pixelWidth/ {print $2}')"
    height="$(sips -g pixelHeight "$frame" 2>/dev/null | awk '/pixelHeight/ {print $2}')"
    DECODE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/c6-camera-decoded.XXXXXX")"
    sips -s format png "$frame" --out "$DECODE_DIR/frame.png" >/dev/null
    rm -rf "$DECODE_DIR"
    DECODE_DIR=""
    dimensions="$width $height"
  elif command -v identify >/dev/null 2>&1; then
    dimensions="$(identify -format '%w %h' "$frame")"
  else
    echo "camera_livestream: need macOS sips or ImageMagick identify to decode JPEGs" >&2
    return 1
  fi
  [[ "$dimensions" == "320 240" ]] || {
    echo "camera_livestream: expected 320x240 JPEG, got $dimensions for $frame" >&2
    return 1
  }
}

validate_jpeg "$ARTIFACT_DIR/frame-1.jpg"
validate_jpeg "$ARTIFACT_DIR/frame-2.jpg"
if cmp -s "$ARTIFACT_DIR/frame-1.jpg" "$ARTIFACT_DIR/frame-2.jpg"; then
  echo "camera_livestream: independently captured frames are byte-identical" >&2
  exit 1
fi

echo "camera_livestream: PASS health + two changing 320x240 JPEG captures"
echo "camera_livestream: URL http://${BOARD_IP}/"
echo "camera_livestream: artifacts $ARTIFACT_DIR/frame-1.jpg $ARTIFACT_DIR/frame-2.jpg"
