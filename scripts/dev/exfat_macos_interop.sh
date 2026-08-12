#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Prove that a volume ra8_fs wrote is TRUE exFAT by handing it to a real
# operating system -- macOS -- rather than only to our own code.
#
# The host tests already validate structure with exfatprogs `fsck.exfat` and an
# in-tree spec-derived scanner. Both are checkers. This script closes the other
# half: it MOUNTS the volume with Apple's exFAT driver, lists the directory,
# reads every file back, and runs Apple's own `fsck_exfat`. Three independent
# implementations then agree, and non-ASCII filenames are proved to survive all
# the way to a user-visible listing.
#
# Everything happens on THIS machine and nothing is borrowed. The image is made
# by tools/exfat_mkimage, a host tool that links the real ra8_fs driver and
# nothing else -- no MMIO, no RTOS, no board -- formats a RAM card, creates a
# known set of files (see its k_entries table, which names every one), and
# writes the card out verbatim. So the image is exactly the bytes our driver
# produced, and what you mount is ra8_fs's own work, not a re-encoding of it.
#
# Usage:
#   scripts/dev/exfat_macos_interop.sh                 # build the tool, make an image, verify it
#   scripts/dev/exfat_macos_interop.sh --dir /tmp/x    # also verify any *.img already in a dir
#   scripts/dev/exfat_macos_interop.sh --keep          # leave the generated image behind
#
# Environment:
#   RA8_IMAGE_DIR  where the image is written (default: /tmp/ra8-exfat-interop)
#
# Exit status: 0 when every image mounts, reads and passes fsck_exfat; 1 otherwise.

set -euo pipefail

IMAGE_DIR="${RA8_IMAGE_DIR:-/tmp/ra8-exfat-interop}"
KEEP=0
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --keep) KEEP=1 ;;
    --dir)
      shift
      IMAGE_DIR="${1:?--dir needs a path}"
      ;;
    -h | --help)
      sed -n '4,28p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1 (try --help)" >&2
      exit 2
      ;;
  esac
  shift
done

# --------------------------------------------------------------------------
# Preconditions -- fail loudly rather than silently skipping.
# --------------------------------------------------------------------------
[ "$(uname -s)" = "Darwin" ] || {
  echo "FATAL: this script proves macOS interop and must run on macOS (found $(uname -s))" >&2
  exit 1
}
for tool in hdiutil diskutil fsck_exfat; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "FATAL: required tool not found: $tool" >&2
    exit 1
  }
done

ATTACHED=""
# shellcheck disable=SC2329  # invoked indirectly by the trap below
cleanup() {
  # Never leave a disk image attached, whatever went wrong.
  for dev in $ATTACHED; do hdiutil detach "$dev" -quiet 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

# --------------------------------------------------------------------------
# Build the generator and make the image, right here.
#
# exfat_mkimage links the ra8_fs driver ALONE -- no ra8_core_hal, so no MMIO
# and nothing that needs a board -- which is precisely why a filesystem written
# for a Cortex-M85 compiles and runs on this laptop.
# --------------------------------------------------------------------------
TOOL_DIR="${REPO_ROOT}/tools/exfat_mkimage"
BUILD_DIR="${IMAGE_DIR}/build"
mkdir -p "$IMAGE_DIR"

echo "== building exfat_mkimage from ${TOOL_DIR} =="
if ! cmake -S "$TOOL_DIR" -B "$BUILD_DIR" >/dev/null 2>&1 ||
  ! cmake --build "$BUILD_DIR" >/dev/null 2>&1; then
  echo "FATAL: could not build exfat_mkimage -- rerun the two cmake commands by hand:" >&2
  echo "  cmake -S ${TOOL_DIR} -B ${BUILD_DIR} && cmake --build ${BUILD_DIR}" >&2
  exit 1
fi

GENERATED="${IMAGE_DIR}/ra8_showcase.img"
echo "== creating the volume with ra8_fs =="
"${BUILD_DIR}/exfat_mkimage" "$GENERATED" || {
  echo "FATAL: exfat_mkimage could not build the volume" >&2
  exit 1
}

shopt -s nullglob
images=("$IMAGE_DIR"/*.img)
shopt -u nullglob
[ "${#images[@]}" -gt 0 ] || {
  echo "FATAL: exfat_mkimage produced no .img in ${IMAGE_DIR}" >&2
  exit 1
}

# --------------------------------------------------------------------------
# Verify each image against Apple's exFAT implementation.
# --------------------------------------------------------------------------
printf '\n%-34s %-7s %-6s %-6s %-6s %s\n' IMAGE MOUNT FSTYPE READ FSCK NAMES
printf '%.0s-' $(seq 1 100)
echo

failures=0
controls=0
for img in "${images[@]}"; do
  name="$(basename "$img" .img)"
  out="$(hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage "$img" 2>&1 || true)"

  # Two image shapes exist and both are legitimate: a whole disk carrying an
  # MBR (the filesystem is on slice s1) and a bare partition image (the
  # filesystem is the device itself). Take the attach handle from the first
  # line and the filesystem device from whichever line carries the mountpoint.
  dev="$(echo "$out" | awk 'NF {print $1; exit}')"
  mntline="$(echo "$out" | grep '/Volumes/' | head -1 || true)"
  fsdev="$(echo "$mntline" | awk 'NF {print $1}')"
  mnt="$(echo "$mntline" | grep -o '/Volumes/.*' || true)"
  if [ -n "$dev" ]; then ATTACHED="$ATTACHED $dev"; fi

  if [ -z "$mnt" ]; then
    printf '%-34s %-7s %-6s %-6s %-6s %s\n' "$name" FAIL - - - "did not mount"
    failures=$((failures + 1))
    if [ -n "$dev" ]; then
      hdiutil detach "$dev" -quiet 2>/dev/null || true
      ATTACHED="${ATTACHED/ $dev/}"
    fi
    continue
  fi

  fstype="$(diskutil info "$fsdev" 2>/dev/null | awk -F': *' '/File System Personality/{print $2}')"
  [ "$fstype" = "ExFAT" ] || failures=$((failures + 1))

  # Read every regular file end to end; a name we cannot open is a real failure.
  readok=ok
  while IFS= read -r f; do
    cat "$f" >/dev/null 2>&1 || readok=FAIL
  done < <(find "$mnt" -type f ! -name '._*' 2>/dev/null)
  [ "$readok" = ok ] || failures=$((failures + 1))

  # Apple's own checker, on the raw device so it sees the on-disk bytes.
  #
  # Some images are deliberately corrupt NEGATIVE CONTROLS (a pre-#606 NameHash,
  # a mangling control). For those a clean bill of health is the failure: it
  # would mean the checker cannot see the very fault the image exists to carry.
  fsckout="$(fsck_exfat -n "${fsdev/disk/rdisk}" 2>&1 || true)"
  case "$name" in
    *badhash* | *mangle_ctrl* | *_ctrl) expect_bad=1 ;;
    *) expect_bad=0 ;;
  esac
  if echo "$fsckout" | grep -qi "appears to be OK"; then
    if [ "$expect_bad" -eq 1 ]; then
      fsck=CTRLBAD # control looked clean -- the check has gone blind
      failures=$((failures + 1))
    else
      fsck=ok
    fi
  else
    if [ "$expect_bad" -eq 1 ]; then
      fsck=ctrl-ok # control fired, exactly as intended
      controls=$((controls + 1))
    else
      fsck=FAIL
      failures=$((failures + 1))
    fi
  fi

  entries="$(find "$mnt" -mindepth 1 ! -name '._*' 2>/dev/null | wc -l | tr -d ' ')"
  printf '%-34s %-7s %-6s %-6s %-6s %s\n' "$name" ok "$fstype" "$readok" "$fsck" "${entries} entries"

  # The point of the exercise: show what a human would actually see.
  find "$mnt" -mindepth 1 ! -name '._*' 2>/dev/null |
    sed "s|^${mnt}/|      |" | head -12

  hdiutil detach "$dev" -quiet 2>/dev/null || true
  ATTACHED="${ATTACHED/ $dev/}"
done

if [ "$KEEP" -eq 0 ] && [ -f "$GENERATED" ]; then
  rm -f "$GENERATED"
  rm -rf "${BUILD_DIR:?}"
fi

echo
if [ "$failures" -eq 0 ]; then
  echo "PASS: ${#images[@]} image(s) -- macOS mounted every volume ra8_fs wrote, read"
  echo "      every file back, and Apple's fsck_exfat passed each volume."
  if [ "$controls" -gt 0 ]; then
    echo "      ${controls} deliberately-corrupt control(s) FIRED as intended (ctrl-ok), so"
    echo "      the check is demonstrably not vacuous."
  fi
  exit 0
fi
echo "FAIL: ${failures} check(s) failed across ${#images[@]} image(s)" >&2
exit 1
