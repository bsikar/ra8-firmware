#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/check_nsc_cmse.sh -- verify every Non-Secure-Callable veneer in
# libs/ra8_nsc compiles under -mcmse with TrustZone enabled (#54).
#
# `cmse_nonsecure_entry` rejects any veneer whose arguments spill past the
# argument registers (AAPCS r0-r3): "attribute not available to functions with
# arguments passed on the stack". The veneers must pack such calls into <=4
# register-sized parameters (see ra8_nsc_spi_pack_const_t in ra8_nsc_comms.h).
#
# No app currently links the comms/eth NSC veneers, so an over-4-arg veneer
# added or edited there would compile-break only when a TrustZone HIL app
# that links them is finally built -- nothing else notices. This
# gate compiles every ra8_nsc TU under the real Cortex-M85 / hard-float / -mcmse
# ABI so a regression is caught immediately.
#
# FAILS (exit 2) if the cross toolchain is absent. It used to `exit 0` there --
# the exact shape CLAUDE.md forbids ("a gate whose dependency is absent must
# FAIL, not pass"), and the one this repo has been bitten by before. The gate
# body in scripts/ci/gates/analysis.sh already requires the compiler before
# calling this, so the change costs nothing in CI; what it buys is that any
# OTHER caller -- a developer running it directly, a future gate -- can no
# longer receive a clean bill of health from a run that compiled nothing.
#
#
set -euo pipefail

mode="scan"
case "$#" in
  0) ;;
  1)
    [[ "$1" == "--selftest" ]] || {
      echo "usage: check_nsc_cmse.sh [--selftest]" >&2
      exit 2
    }
    mode="selftest"
    ;;
  *)
    echo "usage: check_nsc_cmse.sh [--selftest]" >&2
    exit 2
    ;;
esac

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

GCC="${ARM_GCC:-arm-none-eabi-gcc}"
if ! command -v "$GCC" >/dev/null 2>&1; then
  echo "check_nsc_cmse: FATAL -- $GCC is not on PATH." >&2
  echo "  This gate is the only automated guard on the TrustZone Secure-Gateway" >&2
  echo "  ABI, and it cannot run without the cross compiler. It FAILS rather" >&2
  echo "  than skipping: a gate that compiled nothing has not passed." >&2
  echo "  Install the Arm GNU Toolchain, or set ARM_GCC to its arm-none-eabi-gcc." >&2
  exit 2
fi

# C23 standard flag: gcc >= 14 spells it `gnu23`, gcc <= 13 only accepts the
# older `gnu2x` alias. CMake's CMAKE_C_STANDARD 23 in the real build already
# picks whichever the toolchain supports, so probe the same way here instead
# of hardcoding (the CI runner's arm-none-eabi-gcc is gcc 13 -> gnu2x).
cstd="-std=gnu2x"
if printf 'int main(void){return 0;}\n' | "$GCC" -std=gnu23 -xc -fsyntax-only - >/dev/null 2>&1; then
  cstd="-std=gnu23"
fi

# Match the real firmware ABI (cmake/toolchain-ra8d2.cmake) so the cmse
# argument-register rules are evaluated exactly as the app build sees them.
# -Wall -Wextra -Werror matches the firmware warning profile. It is what makes
# this gate catch a redefinition of RA8_NSC_VENEER: a plain -fsyntax-only run
# demotes that to a warning, and the losing definition can silently strip
# cmse_nonsecure_entry (dropping the secure-gateway veneer) while this check
# still reports OK.
flags=(-mcpu=cortex-m85 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
  -mcmse "$cstd" -DRA8_TRUSTZONE_ENABLE -Wall -Wextra -Werror -fsyntax-only)

selftest_cmse() {
  local scratch good bad
  local -a selftest_flags
  scratch="$(mktemp -d)"
  trap 'rm -rf "$scratch"' RETURN
  good="$scratch/good.c"
  bad="$scratch/bad.c"
  cat >"$good" <<'EOF'
__attribute__((cmse_nonsecure_entry))
int good_veneer(int a, int b, int c, int d) { return a + b + c + d; }
EOF
  cat >"$bad" <<'EOF'
__attribute__((cmse_nonsecure_entry))
int bad_veneer(int a, int b, int c, int d, int e) { return a + b + c + d + e; }
EOF
  # The detector is the compiler's CMSE argument-register check, which is
  # architecture-independent within Armv8-M.  Use Cortex-M33 here so this
  # isolated selftest also runs on older Arm GNU releases that predate M85;
  # the production scan below deliberately retains the real M85 ABI flags.
  selftest_flags=(-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
    -mcmse "$cstd" -DRA8_TRUSTZONE_ENABLE -Wall -Wextra -Werror -fsyntax-only)
  if ! "$GCC" "${selftest_flags[@]}" "$good" >/dev/null 2>&1; then
    echo "check_nsc_cmse --selftest: legal four-register veneer was rejected" >&2
    return 1
  fi
  if "$GCC" "${selftest_flags[@]}" "$bad" >/dev/null 2>&1; then
    echo "check_nsc_cmse --selftest: stack-spilling five-argument veneer passed" >&2
    return 1
  fi
  echo "check_nsc_cmse --selftest: PASS (four-register pass, stack-spill reject)"
}

if [[ "$mode" == "selftest" ]]; then
  selftest_cmse
  exit $?
fi

incs=()
for d in libs/*/inc; do
  [ -d "$d" ] && incs+=("-I$d")
done
for d in port/threadx/inc port/usbx/inc port/threadx port/usbx libs/third_party; do
  [ -d "$d" ] && incs+=("-I$d")
done

fail=0
errf="$(mktemp)"
for f in libs/ra8_nsc/src/*.c; do
  if "$GCC" ${flags[@]+"${flags[@]}"} ${incs[@]+"${incs[@]}"} "$f" 2>"$errf"; then
    printf '  OK    %s\n' "$(basename "$f")"
  else
    printf '  FAIL  %s\n' "$(basename "$f")"
    grep -E 'error:' "$errf" | sed 's/^/        /' | head -4
    fail=1
  fi
done
rm -f "$errf"

if [ "$fail" -eq 0 ]; then
  echo "check_nsc_cmse: all NSC veneers compile under -mcmse (TrustZone-on)"
  exit 0
fi
echo "check_nsc_cmse: FAILURES -- a veneer's args spill past r0-r3; pack them (see ra8_nsc_comms.h)"
exit 1
