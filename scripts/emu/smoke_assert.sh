#!/usr/bin/env bash
# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/emu/smoke_assert.sh -- generic run assertions shared by every app class.
#
# SOURCED, NEVER EXECUTED. scripts/emu/smoke.sh is the only entry point; it
# sets ROOT / emu_dir / emu before sourcing, and owns the app list, the build
# phase and the summary.

# A "halt loop" is any firmware give-up spin: the per-app <app>_panic_halt, the
# shared lcd_panic_halt / ra8_exception_halt_loop, or a fault trap
# (HardFault_Handler / Default_Handler). A final PC inside one means a bring-up
# step failed and the firmware gave up. Resolve their address ranges from the
# ELF (nm -S yields each symbol's size) so the check is accurate per app rather
# than a hard-coded address window (which only happened to fit the single-file
# display demos). Pure bash + arm-none-eabi-nm; no gawk-only strtonum.
pc_in_halt_loop() { # elf pcval -> 0 (true) if PC is inside a halt/fault symbol
  local elf="$1" pcval="$2" addr size _type name lo hi
  while read -r addr size _type name; do
    [ -z "$name" ] && continue # 3-field (sizeless) line: skip
    case "$name" in
      *panic_halt | *_halt_loop | HardFault_Handler | Default_Handler) ;;
      *) continue ;;
    esac
    lo=$((16#$addr))
    size=$((16#$size))
    [ "$size" -eq 0 ] && size=4
    hi=$((lo + size))
    if [ "$pcval" -ge "$lo" ] && [ "$pcval" -lt "$hi" ]; then
      return 0
    fi
  done < <(arm-none-eabi-nm -nS "$elf" 2>/dev/null)
  return 1
}

# Count distinct RGB pixels in a P6 PPM. A blank / failed render collapses to a
# handful of colors; a real UI frame has dozens+. Python 3 only, so the check
# runs identically on macOS and the Linux runner.
count_ppm_colors() { # ppm-path -> distinct color count on stdout (0 on error)
  python3 - "$1" 2>/dev/null <<'PY' || echo 0
import pathlib, sys
d = pathlib.Path(sys.argv[1]).read_bytes()
if d[:2] != b"P6":
    print(0); sys.exit(0)
i, t = 2, []
while len(t) < 3:
    while i < len(d) and d[i:i + 1].isspace():
        i += 1
    s = i
    while i < len(d) and not d[i:i + 1].isspace():
        i += 1
    t.append(int(d[s:i]))
i += 1
w, h, _ = t
px = d[i:i + w * h * 3]
print(len({px[o:o + 3] for o in range(0, len(px) - 2, 3)}))
PY
}
