#!/usr/bin/env bash
#
# scripts/board_sim_smoke.sh -- boot each display example on the board emulator
# and assert it runs to its main loop without faulting.
#
# For every app it builds the firmware .elf and runs tools/board_sim headlessly,
# then checks the run: no invalid opcode, no unmapped access, the firmware
# reached the run budget, and the final PC is NOT parked in the lcd_panic_halt
# loop. A fast regression gate for the emulator + the display bring-up path
# (clocks / SDRAM / GLCDC). It does not assert pixel content -- that is checked
# per-app elsewhere; this is the "does it boot and run" smoke layer.
#
#   scripts/board_sim_smoke.sh                 # default display apps
#   scripts/board_sim_smoke.sh blink lcd_draw_x  # explicit app list
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
sim_dir="$ROOT/tools/board_sim"

# A "halt loop" is any firmware give-up spin: the per-app <app>_panic_halt, the
# shared lcd_panic_halt / ra_exception_halt_loop, or a fault trap
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

apps=("$@")
if [ "${#apps[@]}" -eq 0 ]; then
    # Known-good apps that exercise the modelled peripherals: GLCDC (display),
    # GR1 framebuffer, GPIO LED, SCI UART, GPT+ICU IRQ, and USB CDC.
    apps=(blink lcd_color_cycle display_pal_animation bedroom_ui_panel \
        uart_hello gpt_irq_demo usb_cdc_echo)
fi

echo "board_sim smoke: building the emulator ..."
cmake -B "$sim_dir/build" -S "$sim_dir" >/dev/null
cmake --build "$sim_dir/build" -j >/dev/null
sim="$sim_dir/build/board_sim"

fail=0
for app in "${apps[@]}"; do
    printf '  %-24s ' "$app"
    if ! make "$app" >"/tmp/smoke_build_$app.log" 2>&1; then
        echo "BUILD FAIL (see /tmp/smoke_build_$app.log)"
        fail=1
        continue
    fi
    elf="$(find examples -path "*/$app/build/$app.elf" 2>/dev/null | head -1)"
    if [ -z "$elf" ]; then
        echo "NO ELF"
        fail=1
        continue
    fi
    out="$("$sim" "$elf" 2>&1 || true)"
    if echo "$out" | grep -q "INVALID INSN\|UNMAPPED"; then
        echo "FAULT (invalid opcode / unmapped access)"
        fail=1
        continue
    fi
    if ! echo "$out" | grep -q "EXECUTED to the run budget"; then
        echo "DID NOT REACH THE RUN BUDGET"
        fail=1
        continue
    fi
    pc="$(echo "$out" | sed -n 's/.*final PC *: *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1)"
    pcval=$((pc))
    if pc_in_halt_loop "$elf" "$pcval"; then
        echo "PANIC-HALT (pc=$pc)"
        fail=1
        continue
    fi
    echo "OK (pc=$pc)"
done

if [ "$fail" -eq 0 ]; then
    echo "board_sim smoke: ALL PASS"
    exit 0
fi
echo "board_sim smoke: FAILURES"
exit 1
