#!/usr/bin/env bash
#
# scripts/utils/check_nsc_cmse.sh -- verify every Non-Secure-Callable veneer in
# libs/ra_nsc compiles under -mcmse with TrustZone enabled (#54).
#
# `cmse_nonsecure_entry` rejects any veneer whose arguments spill past the
# argument registers (AAPCS r0-r3): "attribute not available to functions with
# arguments passed on the stack". The veneers must pack such calls into <=4
# register-sized parameters (see ra_nsc_spi_pack_const_t in ra_nsc_comms.h).
#
# No app currently links the comms/eth NSC veneers, so an over-4-arg veneer
# added or edited there would compile-break only when a TrustZone HIL app
# (tz_secure_only_da16600_*, ...) is finally built -- nothing else notices. This
# gate compiles every ra_nsc TU under the real Cortex-M85 / hard-float / -mcmse
# ABI so a regression is caught immediately.
#
# Skips cleanly (exit 0) if the cross toolchain is absent, so it is a no-op on
# hosts without arm-none-eabi-gcc; CI runners (and the dev box) have it.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

GCC="${ARM_GCC:-arm-none-eabi-gcc}"
if ! command -v "$GCC" >/dev/null 2>&1; then
    echo "check_nsc_cmse: $GCC not found -- skipping (-mcmse gate needs the cross toolchain)"
    exit 0
fi

# Match the real firmware ABI (cmake/toolchain-ra8d2.cmake) so the cmse
# argument-register rules are evaluated exactly as the app build sees them.
flags=(-mcpu=cortex-m85 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
       -mcmse -std=gnu23 -DRA_TRUSTZONE_ENABLE -fsyntax-only)

incs=()
for d in libs/*/inc; do
    [ -d "$d" ] && incs+=("-I$d")
done
[ -d src/secure_app ] && incs+=("-Isrc/secure_app")
for d in port/threadx port/usbx libs/third_party; do
    [ -d "$d" ] && incs+=("-I$d")
done

fail=0
errf="$(mktemp)"
for f in libs/ra_nsc/src/*.c; do
    if "$GCC" "${flags[@]}" "${incs[@]}" "$f" 2>"$errf"; then
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
echo "check_nsc_cmse: FAILURES -- a veneer's args spill past r0-r3; pack them (see ra_nsc_comms.h)"
exit 1
