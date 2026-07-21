#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_find_jlink.sh -- print the serial number(s) of connected SEGGER J-Link
# probes, including the on-board J-Link OB on an EK-RA8D2. Copy the value into
# JLINK_SN in your .env (see .env.example).
#
# Detection order:
#   1. JLinkExe ShowEmuList -- the SEGGER tools' own probe list (most reliable,
#      cross-platform; this is the serial JLinkExe/rfp-cli expect).
#   2. USB enumeration fallback for when the SEGGER tools are not installed:
#      system_profiler on macOS, /sys on Linux (SEGGER USB vendor id = 1366).
#
# Usage:
#   bash scripts/hil/find_jlink.sh
#   make hil-find-jlink
set -uo pipefail

CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
tag() { printf "${CYAN}[find-jlink]${NC} %s\n" "$*"; }

emit() { # emit <serial>
  [ -n "${1:-}" ] || return 0
  printf "${GREEN}JLINK_SN=%s${NC}\n" "$1"
  FOUND=1
}

FOUND=0

# 1. Ask the SEGGER tools directly.
if command -v JLinkExe >/dev/null 2>&1; then
  tag "querying JLinkExe (ShowEmuList)..."
  out="$(printf 'ShowEmuList\nexit\n' | JLinkExe -nogui 1 2>/dev/null || true)"
  # Lines read: "J-Link[0]: Connection: USB, Serial number: 1086567198, ..."
  while IFS= read -r sn; do emit "$sn"; done < <(
    printf '%s\n' "$out" | grep -oE 'Serial number: [0-9]+' | grep -oE '[0-9]+'
  )
fi

# 2. Fallback: enumerate USB directly (SEGGER vendor id = 1366).
if [ "$FOUND" -eq 0 ]; then
  case "$(uname -s)" in
    Darwin)
      tag "JLinkExe not found; scanning USB via system_profiler..."
      while IFS= read -r sn; do emit "$sn"; done < <(
        system_profiler SPUSBDataType 2>/dev/null |
          awk '/J-Link/{f=1} f&&/Serial Number:/{print $NF; f=0}'
      )
      ;;
    Linux)
      tag "JLinkExe not found; scanning /sys for SEGGER (vendor 1366)..."
      for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] || continue
        [ "$(cat "$d/idVendor" 2>/dev/null)" = "1366" ] || continue
        [ -r "$d/serial" ] && emit "$(cat "$d/serial" 2>/dev/null)"
      done
      ;;
    *)
      tag "unsupported OS for USB fallback; install the SEGGER J-Link tools."
      ;;
  esac
fi

if [ "$FOUND" -eq 0 ]; then
  printf '%sNo J-Link probe found.%s Check the USB cable and board power, then retry.\n' \
    "${YELLOW}" "${NC}" >&2
  printf "The serial is also shown in Ozone / J-Flash Lite (the emulator picker lists it).\n" >&2
  exit 1
fi
