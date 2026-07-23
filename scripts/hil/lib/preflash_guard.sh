#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# preflash_guard.sh -- the anti-recovery pre-flash safety gate.
#
# Owner policy (2026-07-23): every script that programs the physical board runs
# this BEFORE the JLinkExe / rfp-cli program step, so a bad image can never
# brick the owner's board -- even one that came from outside the tree. `source`
# this (do not execute it) and call `ra8_preflash_guard <image.hex|.elf>`
# immediately before programming; if it returns non-zero, the caller MUST exit
# without flashing.
#
# Two layers, either of which aborts the flash:
#   1. SOURCE check -- scripts/checks/check_no_antirecovery.py over the tree
#      (fast; confirms no anti-recovery ACTION was committed). Skipped with a
#      warning only when the caller is not inside a git checkout (a release
#      tarball); the image check below still runs.
#   2. IMAGE check -- scripts/checks/check_image_no_antirecovery.py inspects the
#      ACTUAL bytes about to be flashed and refuses a lockdown value in the
#      disable-initialize / terminal-DLM-lock / permanent-block-protect /
#      HUK-zeroize option-setting region. Benign OFS0/OFS1/SAS/BPS are allowed.
#
# Override (loud, explicit, default REFUSE): set RA8_ALLOW_ANTIRECOVERY_FLASH=1
# to proceed anyway when you are DELIBERATELY provisioning a board and accept
# the brick risk. The guard prints a prominent warning and returns 0.
#
# Portability: pure bash 3.2 (the macOS system bash).

# Layer 1: SOURCE. Scan the tree for a committed anti-recovery ACTION.
#   $1 root  $2 python  $3 source-checker path
#   returns 0 clean/skipped, 1 violation, 3 fatal (missing checker)
_ra8_preflash_source_layer() {
  local root="$1" py="$2" src_check="$3" src_out
  if [ ! -f "$src_check" ]; then
    printf '[preflash-guard] FATAL -- missing source checker: %s\n' "$src_check" >&2
    return 3
  fi
  if src_out="$(cd "$root" && "$py" "$src_check" 2>&1)"; then
    printf '[preflash-guard] layer 1 (source): clean.\n' >&2
    return 0
  fi
  if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf '[preflash-guard] layer 1 (source): FAILED --\n%s\n' "$src_out" >&2
    return 1
  fi
  printf '[preflash-guard] layer 1 (source): SKIPPED (not a git checkout); ' >&2
  printf 'relying on the image check below.\n' >&2
  return 0
}

# Layer 2: IMAGE. Inspect the actual bytes for a lockdown option-byte write.
#   $1 python  $2 image-checker path  $3 image path
#   returns 0 clean, 1 lockdown found, 3 fatal (missing checker / image)
_ra8_preflash_image_layer() {
  local py="$1" img_check="$2" image="$3"
  if [ ! -f "$img_check" ]; then
    printf '[preflash-guard] FATAL -- missing image checker: %s\n' "$img_check" >&2
    return 3
  fi
  if [ ! -f "$image" ]; then
    printf '[preflash-guard] FATAL -- image not found: %s\n' "$image" >&2
    return 3
  fi
  if "$py" "$img_check" "$image" >&2; then
    printf '[preflash-guard] layer 2 (image): clean.\n' >&2
    return 0
  fi
  return 1
}

# Verdict: honour the loud, explicit override, otherwise refuse.
#   $1 image  $2 failed(0/1)   returns 0 proceed, 1 refuse
_ra8_preflash_verdict() {
  local image="$1" failed="$2"
  if [ "$failed" -eq 0 ]; then
    printf '[preflash-guard] OK -- safe to flash %s\n' "$image" >&2
    return 0
  fi
  if [ "${RA8_ALLOW_ANTIRECOVERY_FLASH:-0}" = "1" ]; then
    printf '\n' >&2
    printf '[preflash-guard] ****************************************************************\n' >&2
    printf '[preflash-guard] *** RA8_ALLOW_ANTIRECOVERY_FLASH=1 -- OVERRIDING the refusal ***\n' >&2
    printf '[preflash-guard] *** This image can PERMANENTLY BRICK the board. Proceeding.  ***\n' >&2
    printf '[preflash-guard] ****************************************************************\n' >&2
    return 0
  fi
  printf '\n[preflash-guard] REFUSED -- NOT flashing %s\n' "$image" >&2
  printf '[preflash-guard] (deliberate provisioning only: re-run with ' >&2
  printf 'RA8_ALLOW_ANTIRECOVERY_FLASH=1 to override, at your own risk.)\n' >&2
  return 1
}

# ra8_preflash_guard <image_path>
#   returns 0  -> safe to flash (or override engaged)
#   returns 1  -> REFUSED (an anti-recovery action / lockdown was found)
#   returns 3  -> guard could not run (missing checker / image) -- fail closed
ra8_preflash_guard() {
  local image="${1:-}"
  if [ -z "$image" ]; then
    printf '[preflash-guard] FATAL -- no image path given to ra8_preflash_guard.\n' >&2
    return 3
  fi

  local lib_dir root py src_check img_check failed=0 rc
  lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  root="$(cd "$lib_dir/../../.." && pwd)"
  py="${RA8_PYTHON:-python3}"
  src_check="$root/scripts/checks/check_no_antirecovery.py"
  img_check="$root/scripts/checks/check_image_no_antirecovery.py"

  printf '[preflash-guard] anti-recovery pre-flash checks (image=%s)\n' "$image" >&2

  _ra8_preflash_source_layer "$root" "$py" "$src_check"
  rc=$?
  [ "$rc" -eq 3 ] && return 3
  [ "$rc" -ne 0 ] && failed=1

  _ra8_preflash_image_layer "$py" "$img_check" "$image"
  rc=$?
  [ "$rc" -eq 3 ] && return 3
  [ "$rc" -ne 0 ] && failed=1

  _ra8_preflash_verdict "$image" "$failed"
}
