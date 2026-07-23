#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/check_unicorn_version.sh -- FAIL a board_sim gate when the
# Unicorn the emulator will link is not the pinned version (scripts/ci/unicorn_pin.sh).
#
# WHY (#354): board_sim boots the real firmware .elf on Unicorn, and different
# Unicorn versions decode Armv8.1-M (Helium/MVE) differently, so an unpinned
# Unicorn makes "same commit, different verdict" structural. The provisioning
# guard that created the skew (`if ! ldconfig | grep libunicorn; then apt ...`)
# only checked that SOMETHING named libunicorn existed, never which version --
# so a fossil install produced green board_sim runs nobody could reproduce.
#
# This is the gate-honesty half of the fix: rather than silently install
# whatever apt offers, EVERY board_sim gate calls this check first and fails
# loudly if the runtime Unicorn is not the pin. A skewed or absent Unicorn can
# then never quietly produce a garbage pass again -- the same shape as
# require_cmd / require_python_mod failing loudly on a missing tool.
#
# It binds the ACTUAL library board_sim will use: it compiles a tiny probe
# against <unicorn/unicorn.h> and runs uc_version() through the resolved
# libunicorn.so, then cross-checks the header macros and pkg-config. All three
# must agree with the pin.
#
# Usage:
#   scripts/checks/check_unicorn_version.sh            # enforce; exit non-zero on skew
#   scripts/checks/check_unicorn_version.sh --selftest # prove the check fires both ways

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
# shellcheck source=scripts/ci/unicorn_pin.sh
source "$repo_root/scripts/ci/unicorn_pin.sh"

pin="$RA8_UNICORN_VERSION"

# --- pure comparison, used by both the live check and the selftest -----------
# Args: pin  header_ver(x.y.z|"") runtime_mm(x.y|"") pkgconfig_ver(x.y.z|"")
# Echoes the mismatch reasons (one per line) and returns 1 if any; else 0.
unicorn_version_mismatches() {
  local want="$1" hdr="$2" rt="$3" pc="$4"
  local want_mm="${want%.*}"
  local bad=0
  if [[ -n "$rt" && "$rt" != "$want_mm" ]]; then
    echo "runtime uc_version() reports $rt, expected $want_mm"
    bad=1
  fi
  if [[ -n "$hdr" && "$hdr" != "$want" ]]; then
    echo "compile-time header reports $hdr, expected $want"
    bad=1
  fi
  if [[ -n "$pc" && "$pc" != "$want" ]]; then
    echo "pkg-config --modversion reports $pc, expected $want"
    bad=1
  fi
  if [[ -z "$rt" && -z "$hdr" && -z "$pc" ]]; then
    echo "could not determine the installed Unicorn version at all"
    bad=1
  fi
  return "$bad"
}

# --- probe the real library --------------------------------------------------
# Echoes two whitespace-separated fields: "<header_ver> <runtime_mm>". A field
# that could not be determined is the literal "NA" (never empty, so `read`
# cannot slide the runtime value into the header field).
probe_unicorn() {
  local cc="${CC:-cc}"
  command -v "$cc" >/dev/null 2>&1 || cc=gcc
  command -v "$cc" >/dev/null 2>&1 || {
    echo "NA NA"
    return 0
  }

  local tmp probe bin cflags libs
  tmp="$(mktemp -d)"
  probe="$tmp/uc_probe.c"
  bin="$tmp/uc_probe"

  cat >"$probe" <<'EOF'
#include <stdio.h>
#include <unicorn/unicorn.h>
int main(void) {
  unsigned int maj = 0, min = 0;
  (void)uc_version(&maj, &min);
#if defined(UC_VERSION_MAJOR) && defined(UC_VERSION_MINOR) && defined(UC_VERSION_PATCH)
  printf("%u.%u.%u %u.%u\n",
         (unsigned)UC_VERSION_MAJOR, (unsigned)UC_VERSION_MINOR,
         (unsigned)UC_VERSION_PATCH, maj, min);
#else
  printf("NA %u.%u\n", maj, min);
#endif
  return 0;
}
EOF

  # Prefer pkg-config flags (authoritative; both the dev box and the runner
  # carry a unicorn.pc alongside the pinned /usr/local install). Fall back to
  # the pinned prefix so the probe still binds the same library CMake's
  # find_library(unicorn) picks when pkg-config is absent.
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists unicorn 2>/dev/null; then
    cflags="$(pkg-config --cflags unicorn 2>/dev/null || true)"
    libs="$(pkg-config --libs unicorn 2>/dev/null || echo -lunicorn)"
  else
    cflags="-I${RA8_UNICORN_PREFIX}/include"
    libs="-L${RA8_UNICORN_PREFIX}/lib -Wl,-rpath,${RA8_UNICORN_PREFIX}/lib -lunicorn"
  fi

  # shellcheck disable=SC2086
  if ! "$cc" $cflags "$probe" -o "$bin" $libs >/dev/null 2>&1; then
    # Retry with a bare -lunicorn (default search path) before giving up.
    # shellcheck disable=SC2086
    if ! "$cc" "$probe" -o "$bin" -lunicorn >/dev/null 2>&1; then
      rm -rf "$tmp"
      echo "NA NA"
      return 0
    fi
  fi

  local out
  out="$("$bin" 2>/dev/null || echo "NA NA")"
  rm -rf "$tmp"
  echo "$out"
}

pkgconfig_version() {
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists unicorn 2>/dev/null; then
    pkg-config --modversion unicorn 2>/dev/null || true
  fi
}

# --- selftest: prove the comparison FAILS on skew and PASSES on a match ------
if [[ "${1:-}" == "--selftest" ]]; then
  fails=0
  # unicorn_version_mismatches returns 0 == "match", non-zero == "skew". Map
  # that to a word so the assertion reads without a double negative.
  selftest_case() { # label expect(match|skew) pin hdr rt pc
    local label="$1" expect="$2"
    shift 2
    local got
    if unicorn_version_mismatches "$@" >/dev/null 2>&1; then
      got="match"
    else
      got="skew"
    fi
    if [[ "$got" == "$expect" ]]; then
      echo "[selftest] OK: $label"
    else
      echo "[selftest] FAIL: $label (expected $expect, got $got)"
      fails=1
    fi
  }
  # Direction 1: the 2.0.1 fossil against a 2.1.4 pin MUST read as a skew (the
  # exact bug this check exists to catch).
  selftest_case "skew 2.0.1 vs pin 2.1.4 is detected" skew "2.1.4" "2.0.1" "2.0" "2.0.1"
  # Direction 2: the exact pin MUST read as a match.
  selftest_case "matching pin 2.1.4 is accepted" match "2.1.4" "2.1.4" "2.1" "2.1.4"
  # Direction 3: a total absence of Unicorn MUST read as a skew, never a match.
  selftest_case "an undetectable Unicorn is a failure" skew "2.1.4" "" "" ""
  # Direction 4: a patch-level drift (2.1.3 vs 2.1.4) via pkg-config MUST fail.
  selftest_case "patch drift 2.1.3 vs 2.1.4 is detected" skew "2.1.4" "2.1.4" "2.1" "2.1.3"
  if [[ "$fails" -ne 0 ]]; then
    echo "[selftest] check_unicorn_version.sh selftest FAILED" >&2
    exit 1
  fi
  echo "[selftest] check_unicorn_version.sh selftest passed"
  exit 0
fi

# --- live enforcement --------------------------------------------------------
read -r hdr_ver rt_mm < <(probe_unicorn)
[[ "$hdr_ver" == "NA" ]] && hdr_ver=""
[[ "$rt_mm" == "NA" ]] && rt_mm=""
pc_ver="$(pkgconfig_version)"

echo "board_sim Unicorn pin check:"
echo "  pinned          : $pin (scripts/ci/unicorn_pin.sh)"
echo "  header (compile): ${hdr_ver:-<none>}"
echo "  uc_version (run): ${rt_mm:-<none>}"
echo "  pkg-config      : ${pc_ver:-<none>}"

if reasons="$(unicorn_version_mismatches "$pin" "$hdr_ver" "$rt_mm" "$pc_ver")"; then
  echo "  result          : OK -- Unicorn matches the pin."
  exit 0
fi

{
  echo ""
  echo "ERROR: the installed Unicorn does not match the pinned version."
  while IFS= read -r r; do echo "  - $r"; done <<<"$reasons"
  echo ""
  echo "board_sim decodes Armv8.1-M (Helium/MVE) differently across Unicorn"
  echo "versions, so this gate refuses to run on an unpinned emulator (#354)."
  echo ""
  echo "Fix it by installing the pinned build:"
  echo "  bash scripts/ci/install_unicorn.sh           # -> \$RA8_UNICORN_PREFIX (/usr/local)"
  echo "On a self-hosted runner, provision the same pin (docs/TOOLCHAIN.md)."
} >&2
exit 1
