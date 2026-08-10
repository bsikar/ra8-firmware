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
# The host test binaries cannot run on macOS (they mmap peripheral RAM with
# MAP_FIXED below 4 GiB, which macOS arm64 refuses), so image GENERATION runs on
# the Linux dev box over ssh and the images are copied here for VERIFICATION.
#
# Usage:
#   scripts/dev/exfat_macos_interop.sh                 # verify images already in the dir
#   scripts/dev/exfat_macos_interop.sh --generate      # regenerate on the dev box, then verify
#   scripts/dev/exfat_macos_interop.sh --dir /tmp/x    # use a different image directory
#
# Environment:
#   RA8_DEV_HOST   ssh host that builds the images (default: dev)
#   RA8_IMAGE_DIR  where images live on this Mac    (default: /tmp/ra8-exfat-interop)
#
# Exit status: 0 when every image mounts, reads and passes fsck_exfat; 1 otherwise.

set -euo pipefail

DEV_HOST="${RA8_DEV_HOST:-dev}"
IMAGE_DIR="${RA8_IMAGE_DIR:-/tmp/ra8-exfat-interop}"
GENERATE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --generate) GENERATE=1 ;;
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
# Optionally regenerate the evidence images on the dev box.
# --------------------------------------------------------------------------
if [ "$GENERATE" -eq 1 ]; then
  echo "== generating images on ${DEV_HOST} =="
  ssh "$DEV_HOST" 'set -e
    cd ~/ra8-firmware
    git fetch -q origin
    rm -rf ~/ra8-ws/exfat-interop
    git worktree add --detach ~/ra8-ws/exfat-interop origin/dev >/dev/null 2>&1
    cd ~/ra8-ws/exfat-interop
    bash tests/build_tests.sh >/dev/null 2>&1
    rm -rf /tmp/ra8-exfat-interop && mkdir -p /tmp/ra8-exfat-interop
    cd tests/build
    for t in test_ra8_fs_unicode_exfat test_ra8_fs_unicode_exfat_dirs \
             test_ra8_fs_unicode_exfat_cov test_ra8_fs_exfat_stream \
             test_ra8_fs_format_exfat; do
      [ -x "./$t" ] && RA8_EXFAT_DUMP_DIR=/tmp/ra8-exfat-interop "./$t" >/dev/null 2>&1 || true
    done
    ls /tmp/ra8-exfat-interop/*.img 2>/dev/null | wc -l' | sed 's/^/  images built: /'

  mkdir -p "$IMAGE_DIR"
  rm -f "${IMAGE_DIR:?}"/*.img
  scp -q "${DEV_HOST}:/tmp/ra8-exfat-interop/*.img" "$IMAGE_DIR/"
  echo "  copied to ${IMAGE_DIR}"
fi

shopt -s nullglob
images=("$IMAGE_DIR"/*.img)
shopt -u nullglob
[ "${#images[@]}" -gt 0 ] || {
  echo "FATAL: no .img files in ${IMAGE_DIR} -- run with --generate first" >&2
  exit 1
}

# --------------------------------------------------------------------------
# Verify each image against Apple's exFAT implementation.
# --------------------------------------------------------------------------
printf '\n%-34s %-7s %-6s %-6s %-6s %s\n' IMAGE MOUNT FSTYPE READ FSCK NAMES
printf '%.0s-' $(seq 1 100)
echo

failures=0
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

echo
if [ "$failures" -eq 0 ]; then
  echo "PASS: ${#images[@]} image(s) -- macOS mounted every volume ra8_fs wrote and read"
  echo "      every file back. Apple's fsck_exfat passed each real volume and FIRED on"
  echo "      each deliberately-corrupt control (ctrl-ok), so the check is not vacuous."
  exit 0
fi
echo "FAIL: ${failures} check(s) failed across ${#images[@]} image(s)" >&2
exit 1
