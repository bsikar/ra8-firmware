#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tty_resolve.sh -- resolve a bench serial console to a STABLE device path.
# `source` this (do not execute it), then call `ra8_tty_resolve <kind>`.
#
# Why this exists
# ---------------
# /dev/ttyACM<n> is assignment order, not identity. The bench has three CDC
# devices on it (the EK-RA8D2's J-Link OB VCOM, the ESP32-C6's CH343 UART
# bridge, and the C6's native USB-JTAG port), so a power cycle can renumber
# all of them: on 2026-07-27 ttyACM0 became the C6 and the board console moved
# to ttyACM1, and every script that assumed ttyACM0 read the wrong device --
# silently, because the wrong device is a real tty that simply says nothing.
#
# /dev/serial/by-id/ names devices by what they ARE (vendor, product, serial),
# so it survives renumbering. That namespace is the primary resolution here;
# a udev scan is kept as a fallback for hosts where it is absent.
#
# What this deliberately does NOT do
# ----------------------------------
# Fall back to /dev/ttyACM0. That fallback is what turned a renumbering into a
# day of confusing failures: reads from the wrong device look exactly like a
# board that booted and printed nothing. When identity cannot be established
# this fails loudly instead, and names the override to set.
#
# Kinds:
#   console  the EK-RA8D2 board console (J-Link OB VCOM, SEGGER VID 1366)
#   c6       the ESP32-C6 flash / console bridge (CH343, VID 1a86)
#
# Overrides (checked first, so --uart and .env always win):
#   RA8_CONSOLE_TTY   explicit path for `console`
#   C6_CONSOLE_TTY    explicit path for `c6`
#   JLINK_SN          pins one probe when two J-Links are attached
#
# Portability: bash 3.2 (the macOS system bash) -- no name-refs, no mapfile.
# Self-contained on purpose: the Pi-side halves of these scripts get this file
# piped to them over ssh, so it must not depend on rig_env.sh or on the repo
# being present at the far end.

# USB vendor ids, for the udev fallback path.
RA8_TTY_VID_SEGGER="1366"
RA8_TTY_VID_CH343="1a86"

# /dev/serial/by-id/ globs. The serial number is deliberately NOT baked in:
# matching the device class keeps maintainer-specific serials out of the tree
# (the same reason JLINK_SN lives in .env), and a bench with two of one kind
# gets a clear "pin it" error rather than a coin toss.
RA8_TTY_GLOB_SEGGER="/dev/serial/by-id/usb-SEGGER_J-Link_*-if00"
RA8_TTY_GLOB_CH343="/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00"

# ra8_tty_by_id GLOB [SERIAL_HINT]
# Print the single by-id path matching GLOB. With SERIAL_HINT, only paths
# containing that substring count, which is how a two-probe bench is pinned.
#
# Prints NOTHING when there is no unambiguous match, and still succeeds: "not
# found" is an ordinary outcome of a lookup, not an error. Callers therefore
# test the output rather than the status, and nothing has to wrap the call in
# `|| true` -- which would mask a genuine mid-body failure from errexit.
ra8_tty_by_id() {
  _glob="$1"
  _hint="${2:-}"
  _hits=""
  _count=0
  for _p in $_glob; do
    [ -e "$_p" ] || continue
    case "$_p" in
      *"$_hint"*) ;;
      *) continue ;;
    esac
    _hits="$_hits $_p"
    _count=$((_count + 1))
  done
  if [ "$_count" -eq 1 ]; then
    printf '%s\n' "${_hits# }"
  elif [ "$_count" -gt 1 ]; then
    printf 'tty_resolve: ambiguous -- %s devices match %s:%s\n' \
      "$_count" "$_glob" "$_hits" >&2
  fi
  return 0
}

# ra8_tty_by_vid VID
# Fallback for hosts without /dev/serial/by-id: scan /dev/ttyACM* and print
# the first whose udev record carries VID. Still identity-based -- it just
# costs a udevadm call per device and cannot distinguish two of a kind.
#
# Prints nothing and still succeeds when there is no match, for the reason
# given on ra8_tty_by_id.
ra8_tty_by_vid() {
  _vid="$1"
  command -v udevadm >/dev/null 2>&1 || return 0
  for _d in /dev/ttyACM*; do
    [ -e "$_d" ] || continue
    if udevadm info "$_d" 2>/dev/null | grep -q "ID_VENDOR_ID=${_vid}"; then
      printf '%s\n' "$_d"
      return 0
    fi
  done
  return 0
}

# ra8_tty_inventory
# List every serial device present, by-id name and ttyACM alike, one per line.
# Printed when resolution fails: "which devices ARE here" is the first thing
# anyone asks next, and a renumbering is obvious the moment the list is seen.
ra8_tty_inventory() {
  _seen=0
  for _p in /dev/serial/by-id/* /dev/ttyACM*; do
    [ -e "$_p" ] || continue
    _seen=$((_seen + 1))
    if [ -L "$_p" ]; then
      printf '    %s -> %s\n' "$_p" "$(readlink "$_p")"
    else
      printf '    %s\n' "$_p"
    fi
  done
  [ "$_seen" -eq 0 ] && printf '    (no serial devices at all)\n'
  return 0
}

# ra8_tty_resolve KIND [RETRIES]
# Print the stable device path for KIND, or fail (exit 1) with a diagnosis.
# RETRIES (default 10, ~0.5 s apart) covers the J-Link briefly disappearing
# between back-to-back flash cycles.
ra8_tty_resolve() {
  _kind="$1"
  _tries="${2:-10}"
  case "$_kind" in
    console)
      _override="${RA8_CONSOLE_TTY:-}"
      _glob="$RA8_TTY_GLOB_SEGGER"
      _vid="$RA8_TTY_VID_SEGGER"
      _hint="${JLINK_SN:-}"
      _what="EK-RA8D2 board console (J-Link OB VCOM)"
      _var="RA8_CONSOLE_TTY"
      ;;
    c6)
      _override="${C6_CONSOLE_TTY:-}"
      _glob="$RA8_TTY_GLOB_CH343"
      _vid="$RA8_TTY_VID_CH343"
      _hint=""
      _what="ESP32-C6 CH343 UART bridge"
      _var="C6_CONSOLE_TTY"
      ;;
    *)
      printf 'tty_resolve: unknown kind %s (want: console | c6)\n' "$_kind" >&2
      return 2
      ;;
  esac

  if [ -n "$_override" ]; then
    printf '%s\n' "$_override"
    return 0
  fi

  _n=0
  while [ "$_n" -lt "$_tries" ]; do
    _found="$(ra8_tty_by_id "$_glob" "$_hint")"
    if [ -z "$_found" ]; then
      _found="$(ra8_tty_by_vid "$_vid")"
    fi
    if [ -n "$_found" ]; then
      printf '%s\n' "$_found"
      return 0
    fi
    _n=$((_n + 1))
    [ "$_n" -lt "$_tries" ] && sleep 0.5
  done

  printf 'tty_resolve: %s not found.\n' "$_what" >&2
  printf '  looked for: %s\n' "$_glob" >&2
  printf '  then for a USB device with vendor id %s under /dev/ttyACM*\n' "$_vid" >&2
  printf '  present now:\n' >&2
  ra8_tty_inventory >&2
  printf '  set %s=<path> to override, or check the cable and board power.\n' "$_var" >&2
  return 1
}
