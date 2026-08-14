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
HEX_PATH=""
EXPECTED_WIDTH=320
EXPECTED_HEIGHT=240
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app-dir)
      APP="$2"
      shift 2
      ;;
    --width)
      EXPECTED_WIDTH="$2"
      shift 2
      ;;
    --height)
      EXPECTED_HEIGHT="$2"
      shift 2
      ;;
    --hex)
      HEX_PATH="$2"
      shift 2
      ;;
    -h | --help)
      echo "usage: $0 [--app-dir <path>] [--width <pixels>] [--height <pixels>] [--hex <prebuilt.hex>]"
      exit 0
      ;;
    *)
      echo "camera_livestream: unknown argument '$1'" >&2
      exit 2
      ;;
  esac
done
[[ -d "$APP" ]] || {
  echo "camera_livestream: app directory does not exist: $APP" >&2
  exit 2
}
if [[ -n "$HEX_PATH" && ! -f "$HEX_PATH" ]]; then
  echo "camera_livestream: prebuilt image does not exist: $HEX_PATH" >&2
  exit 2
fi
[[ "$EXPECTED_WIDTH" =~ ^[1-9][0-9]*$ && "$EXPECTED_HEIGHT" =~ ^[1-9][0-9]*$ ]] || {
  echo "camera_livestream: width and height must be positive integers" >&2
  exit 2
}
APP_NAME="$(basename "$APP")"
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
CURRENT_STAGE="argument validation"
RUN_SUCCEEDED=0
mkdir -p "$ARTIFACT_DIR"
rm -f "$ARTIFACT_DIR/board-ip" "$ARTIFACT_DIR/uart.log" \
  "$ARTIFACT_DIR/frame-1.jpg" "$ARTIFACT_DIR/frame-2.jpg" \
  "$ARTIFACT_DIR/audio.wav" "$ARTIFACT_DIR/stream.bin" \
  "$ARTIFACT_DIR/stream.headers" "$ARTIFACT_DIR/stream-frame-1.jpg" \
  "$ARTIFACT_DIR/stream-frame-2.jpg"
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
  if [[ "$RUN_SUCCEEDED" != 1 ]]; then
    echo "camera_livestream: FAIL stage=$CURRENT_STAGE" >&2
    if [[ -s "$LOG" ]]; then
      cp "$LOG" "$ARTIFACT_DIR/uart.log"
      echo "camera_livestream: UART tail follows (full log: $ARTIFACT_DIR/uart.log)" >&2
      tail -40 "$LOG" >&2
    fi
    if [[ "$REMOTE_FILES_CREATED" == 1 ]]; then
      echo "camera_livestream: remote failure artifacts retained at ${PI_HOST:-localhost}:${REMOTE_PREFIX}-*" >&2
    fi
  fi
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
  if [[ "$RUN_SUCCEEDED" != 1 ]]; then
    return 0
  fi
  if run_on_pi "rm -f '${REMOTE_PREFIX}-1.jpg' '${REMOTE_PREFIX}-2.jpg' \
    '${REMOTE_PREFIX}-audio.wav' '${REMOTE_PREFIX}-stream.bin' \
    '${REMOTE_PREFIX}-stream.headers' '${REMOTE_PREFIX}-health.txt' \
    '${REMOTE_PREFIX}-stream-1.jpg' '${REMOTE_PREFIX}-stream-2.jpg'"; then
    REMOTE_FILES_CREATED=0
  fi
  return 0
}
trap cleanup_local EXIT

if [[ -z "$HEX_PATH" ]]; then
  CURRENT_STAGE="firmware build"
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
  HEX_PATH="$BUILD_DIR/${APP_NAME}.hex"
fi
CURRENT_STAGE="bench acquisition"
ra8_bench_require "validate C6 camera livestream" 30m || exit $?
# Run remote cleanup before the guard's EXIT handler releases the bench.
_ra8_bench_add_exit_trap cleanup_remote

echo "camera_livestream: physical precondition: SW4 1=OFF 2=OFF 3=ON 4=OFF; C6 on J26" >&2
c6_ready=false
CURRENT_STAGE="C6 readiness probe"
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
CURRENT_STAGE="firmware startup selftest"
bash "$ROOT/scripts/hil/run_direct.sh" \
  --hex "$HEX_PATH" \
  --expect 'c6_cam: PASS HTTP camera server listening' \
  --expect-negative 'c6_cam: FAIL|HardFault' --timeout 90 | tee "$LOG"

STARTUP_BANNER='c6_cam: camera=PASS '
if ! grep -qF "$STARTUP_BANNER" "$LOG"; then
  echo "camera_livestream: missing startup proof: $STARTUP_BANNER" >&2
  exit 1
fi

CURRENT_STAGE="UART address discovery"
BOARD_IP="$(sed -n 's/.*c6_cam: PASS Wi-Fi and DHCP ip=\([0-9.]*\).*/\1/p' "$LOG" | tail -1)"
[[ "$BOARD_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "camera_livestream: could not parse board IP from UART" >&2
  exit 1
}
printf '%s\n' "$BOARD_IP" >"$ARTIFACT_DIR/board-ip"

REMOTE_FILES_CREATED=1
CURRENT_STAGE="HTTP health, still, audio, and multipart fetch"
run_on_pi "set -euo pipefail
  fail() { echo \"camera_livestream: remote FAIL stage=\$1\" >&2; exit 1; }
  health=${REMOTE_PREFIX}-health.txt
  curl -fsS --max-time 10 'http://${BOARD_IP}/health' -o \"\$health\" || fail health-fetch
  test \"\$(cat \"\$health\")\" = 'PASS c6 camera livestream audio=PASS camera=PASS' || fail health-body
  for n in 1 2; do
    f=${REMOTE_PREFIX}-\${n}.jpg
    curl -fsS --max-time 20 'http://${BOARD_IP}/frame.jpg?probe='\$n -o \"\$f\" || fail still-\${n}-fetch
    test \"\$(od -An -tx1 -N2 \"\$f\" | tr -d ' ')\" = ffd8 || fail still-\${n}-soi
    test \"\$(stat -c %s \"\$f\")\" -gt 1000 || fail still-\${n}-size
    sleep 1
  done
  audio=${REMOTE_PREFIX}-audio.wav
  curl -fsS --max-time 10 'http://${BOARD_IP}/audio.wav' -o \"\$audio\" || fail audio-fetch
  python3 - \"\$audio\" <<'PY' || fail audio-validation
import pathlib
import struct
import sys
import wave

path = pathlib.Path(sys.argv[1])
with wave.open(str(path), 'rb') as stream:
    assert stream.getnchannels() == 1
    assert stream.getsampwidth() == 2
    assert stream.getframerate() == 16000
    frames = stream.getnframes()
    assert 8000 <= frames <= 16000, frames
    pcm = stream.readframes(frames)
assert len(pcm) == frames * 2
samples = struct.unpack(f'<{frames}h', pcm)
minimum = min(samples)
maximum = max(samples)
rms = int((sum(sample * sample for sample in samples) / frames) ** 0.5)
assert maximum - minimum >= 8, (minimum, maximum)
assert len(set(samples)) >= 4
assert rms >= 1, rms
print(f'camera_livestream: audio_frames={frames} rms={rms} span={maximum - minimum}')
PY
  stream=${REMOTE_PREFIX}-stream.bin
  headers=${REMOTE_PREFIX}-stream.headers
  set +e
  curl -fsS --max-time 30 -D \"\$headers\" 'http://${BOARD_IP}/stream.mjpg' -o \"\$stream\"
  curl_rc=\$?
  set -e
  { test \"\$curl_rc\" -eq 0 || test \"\$curl_rc\" -eq 28; } || fail multipart-fetch
  grep -Eqi '^Content-Type: multipart/x-mixed-replace;[[:space:]]*boundary=frame' \"\$headers\" || fail multipart-content-type
  python3 - \"\$stream\" '${REMOTE_PREFIX}-stream-1.jpg' '${REMOTE_PREFIX}-stream-2.jpg' <<'PY' || fail multipart-validation
import pathlib
import sys

payload = pathlib.Path(sys.argv[1]).read_bytes()
frames = []
for part_number, part in enumerate(payload.split(b'--frame')[1:], 1):
    if part.startswith(b'--'):
        break
    if part.startswith(b'\r\n'):
        part = part[2:]
    headers, separator, body = part.partition(b'\r\n\r\n')
    if not separator:
        continue
    fields = {}
    for line in headers.split(b'\r\n'):
        name, delimiter, value = line.partition(b':')
        if delimiter:
            fields[name.strip().lower()] = value.strip()
    if fields.get(b'content-type', b'').lower() != b'image/jpeg':
        raise SystemExit(f'part {part_number}: Content-Type is not image/jpeg')
    try:
        content_length = int(fields[b'content-length'])
    except (KeyError, ValueError) as error:
        raise SystemExit(f'part {part_number}: invalid Content-Length') from error
    if len(body) < content_length:
        continue
    frame = body[:content_length]
    if not frame.startswith(b'\xff\xd8') or not frame.endswith(b'\xff\xd9'):
        raise SystemExit(f'part {part_number}: declared JPEG lacks SOI/EOI')
    frames.append(frame)
    if len(frames) == 2:
        break
if len(frames) < 2:
    raise SystemExit(f'expected two complete multipart frames, got {len(frames)}')
if frames[0] == frames[1]:
    raise SystemExit('first two multipart frames are byte-identical')
pathlib.Path(sys.argv[2]).write_bytes(frames[0])
pathlib.Path(sys.argv[3]).write_bytes(frames[1])
print(f'camera_livestream: multipart_frames=2 bytes={len(payload)}')
PY"

CURRENT_STAGE="artifact collection"
for n in 1 2; do
  copy_from_pi "${REMOTE_PREFIX}-${n}.jpg" "$ARTIFACT_DIR/frame-${n}.jpg"
  copy_from_pi "${REMOTE_PREFIX}-stream-${n}.jpg" "$ARTIFACT_DIR/stream-frame-${n}.jpg"
done
copy_from_pi "${REMOTE_PREFIX}-audio.wav" "$ARTIFACT_DIR/audio.wav"
copy_from_pi "${REMOTE_PREFIX}-stream.bin" "$ARTIFACT_DIR/stream.bin"
copy_from_pi "${REMOTE_PREFIX}-stream.headers" "$ARTIFACT_DIR/stream.headers"

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
  elif command -v magick >/dev/null 2>&1; then
    dimensions="$(magick identify -format '%w %h' "$frame")"
    magick "$frame" null:
  elif command -v identify >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
    dimensions="$(identify -format '%w %h' "$frame")"
    convert "$frame" null:
  elif command -v python3 >/dev/null 2>&1; then
    dimensions="$(
      python3 - "$frame" <<'PY'
import sys

try:
    from PIL import Image
except ImportError as error:
    raise SystemExit('Pillow is required when no native image decoder is installed') from error
with Image.open(sys.argv[1]) as image:
    image.load()
    if image.format != 'JPEG':
        raise SystemExit(f'expected JPEG, got {image.format}')
    print(*image.size)
PY
    )"
  else
    echo "camera_livestream: need macOS sips, ImageMagick, or Python 3 with Pillow to decode JPEGs" >&2
    return 1
  fi
  [[ "$dimensions" == "$EXPECTED_WIDTH $EXPECTED_HEIGHT" ]] || {
    echo "camera_livestream: expected ${EXPECTED_WIDTH}x${EXPECTED_HEIGHT} JPEG, got $dimensions for $frame" >&2
    return 1
  }
}

CURRENT_STAGE="local JPEG decode and dimension validation"
validate_jpeg "$ARTIFACT_DIR/frame-1.jpg"
validate_jpeg "$ARTIFACT_DIR/frame-2.jpg"
validate_jpeg "$ARTIFACT_DIR/stream-frame-1.jpg"
validate_jpeg "$ARTIFACT_DIR/stream-frame-2.jpg"
if cmp -s "$ARTIFACT_DIR/frame-1.jpg" "$ARTIFACT_DIR/frame-2.jpg"; then
  echo "camera_livestream: independently captured frames are byte-identical" >&2
  exit 1
fi
if cmp -s "$ARTIFACT_DIR/stream-frame-1.jpg" "$ARTIFACT_DIR/stream-frame-2.jpg"; then
  echo "camera_livestream: multipart frames are byte-identical" >&2
  exit 1
fi

RUN_SUCCEEDED=1
cleanup_remote
echo "camera_livestream: PASS startup selftest + health + two changing decodable ${EXPECTED_WIDTH}x${EXPECTED_HEIGHT} stills + audio WAV + multipart frames"
echo "camera_livestream: URL http://${BOARD_IP}/"
echo "camera_livestream: artifacts $ARTIFACT_DIR"
