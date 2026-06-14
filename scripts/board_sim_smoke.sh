#!/usr/bin/env bash
#
# scripts/board_sim_smoke.sh -- boot each display example on the board emulator
# and assert it runs to its main loop without faulting.
#
# For every app it builds the firmware .elf and runs tools/board_sim headlessly,
# then checks the run: no invalid opcode, no unmapped access, the firmware
# reached the run budget, and the final PC is NOT parked in the lcd_panic_halt
# loop. A fast regression gate for the emulator + the display bring-up path
# (clocks / SDRAM / GLCDC). For the UI apps in $render_assert_apps it ALSO
# renders one frame to a PPM and asserts the panel drew rich content (a floor on
# distinct colors) -- this folds in the pixel-content check the retired
# ui_render_check.sh used to do against the (now removed) native UI simulator.
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

# UI apps whose rendered frame must be rich (distinct-color floor). ereader_ui
# is the e-reader chrome (ra_box + ra_gfx); this gates that it actually paints.
# sd_font_render reads FONT.OTF off the modelled microSD and reflows it, so its
# frame must also be non-trivial.
render_assert_apps="ereader_ui sd_font_render"
min_render_colors=6

# Apps whose rendered chrome is pinned to a checked-in golden image (exact
# pixel match of the panel framebuffer, a strictly stronger check than the
# distinct-color floor above). board_sim renders deterministically, so any
# unintended chrome change fails here. Regenerate after an intentional change
# with `make ereader-golden-update`. See scripts/utils/ereader_golden.py (#84).
golden_apps="ereader_ui"
golden_dir="$ROOT/tests/golden/ereader_chrome"

# Apps that need a microSD card attached (board_sim --sd <image>). The harness
# auto-builds a small FAT16 card image (a font as FONT.OTF) and passes it so
# sd_font_render can mount + read it. This is how "specify a microSD exists" is
# exercised in CI: drop the app name here and it runs against a modelled card.
sd_apps="sd_font_render"
sd_image=""

# Build a microSD card image (FAT16 + FONT.OTF) for the SD apps. Uses the small
# in-repo ahem.ttf so the whole font reads back within the run budget. Sets the
# global $sd_image on success; leaves it empty (apps still run, just card-less)
# if mkfontimg or the font is unavailable.
build_sd_image() {
    local font="$ROOT/libs/third_party/litehtml/containers/test/fonts/ahem.ttf"
    local mk="$ROOT/tools/mkfontimg"
    [ -f "$font" ] || return 0
    cmake -B "$mk/build" -S "$mk" >/dev/null 2>&1 || return 0
    cmake --build "$mk/build" >/dev/null 2>&1 || return 0
    sd_image="$(mktemp -t board_sim_sd.XXXXXX.img)"
    "$mk/build/mkfontimg" "$font" "$sd_image" FONT.OTF >/dev/null 2>&1 || sd_image=""
}

# Echo the extra board_sim args an app needs (e.g. --sd <image> for SD apps).
sim_extra_args() { # app -> extra args on stdout
    case " $sd_apps " in
    *" $1 "*) [ -n "$sd_image" ] && printf -- '--sd %s' "$sd_image" ;;
    esac
}

apps=("$@")
if [ "${#apps[@]}" -eq 0 ]; then
    # Bare-metal apps that run identically on the CI runner's Unicorn (2.0.1) and
    # newer builds, exercising the modelled peripherals: GLCDC (display), GR1
    # framebuffer, GPIO LED, SCI UART, GPT+ICU IRQ, SSIE (I2S), CRC (crc_demo
    # self-checks hw==sw), DOC (doc_demo matches a software sum), and CAN-FD
    # (canfd_loopback round-trips a frame). The ThreadX/USBX apps (usb_cdc_echo,
    # threadx_usbx_cdc_demo, ...) drive the hand-rolled exception path on the
    # first context switch, which Unicorn 2.0.1 mis-delivers (UC_ERR_EXCEPTION);
    # they run on a newer Unicorn (macOS / a source build) -- pass them
    # explicitly there (e.g. `board_sim_smoke.sh usb_cdc_echo`).
    #
    # The Octo-SPI LevelX/FileX apps (threadx_levelx_demo,
    # threadx_filex_levelx_demo) exercise the xSPI flash model
    # (board_periph_xspi.c) -- LevelX format/open + sector R/W, and FileX FAT
    # file write+readback round-trip. They are ThreadX, so the same Unicorn
    # 2.0.1 caveat applies: pass them explicitly on a newer Unicorn / macOS.
    apps=(blink lcd_color_cycle display_pal_animation ereader_ui \
        uart_hello gpt_irq_demo ssie_audio_loop crc_demo doc_demo \
        canfd_loopback)
fi

echo "board_sim smoke: building the emulator ..."
cmake -B "$sim_dir/build" -S "$sim_dir" >/dev/null
cmake --build "$sim_dir/build" -j >/dev/null
sim="$sim_dir/build/board_sim"

# Build the microSD card image once if any selected app needs it.
for app in "${apps[@]}"; do
    case " $sd_apps " in
    *" $app "*) build_sd_image; break ;;
    esac
done

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
    # shellcheck disable=SC2046  -- intentional word-split of the extra args
    extra="$(sim_extra_args "$app")"
    out="$("$sim" "$elf" $extra 2>&1 || true)"
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
    case " $render_assert_apps " in
    *" $app "*)
        ppm="$(mktemp)"
        # shellcheck disable=SC2046  -- intentional word-split of the extra args
        "$sim" "$elf" --ppm "$ppm" $extra >/dev/null 2>&1 || true
        colors="$(count_ppm_colors "$ppm")"
        rm -f "$ppm"
        if [ "${colors:-0}" -lt "$min_render_colors" ]; then
            echo "RENDER SPARSE (pc=$pc, $colors colors < $min_render_colors)"
            fail=1
            continue
        fi
        case " $golden_apps " in
        *" $app "*)
            if python3 "$ROOT/scripts/utils/ereader_golden.py" check \
                --elf "$elf" --board-sim "$sim" --golden-dir "$golden_dir" \
                --out-dir /tmp/ereader_golden_out >"/tmp/golden_$app.log" 2>&1; then
                echo "OK (pc=$pc, render=$colors colors, golden OK)"
            else
                echo "GOLDEN DRIFT (pc=$pc; see /tmp/golden_$app.log)"
                fail=1
            fi
            continue
            ;;
        esac
        echo "OK (pc=$pc, render=$colors colors)"
        continue
        ;;
    esac
    echo "OK (pc=$pc)"
done

[ -n "$sd_image" ] && rm -f "$sd_image"

if [ "$fail" -eq 0 ]; then
    echo "board_sim smoke: ALL PASS"
    exit 0
fi
echo "board_sim smoke: FAILURES"
exit 1
