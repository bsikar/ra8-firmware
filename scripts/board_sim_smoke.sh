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

# board_sim's run log and `arm-none-eabi-nm` output can carry non-UTF-8 bytes
# (register dumps, the SD font image echoed back). Under a UTF-8 locale BSD sed
# (macOS) aborts those pipelines with "RE error: illegal byte sequence"; force
# the C locale so every text tool here treats input as raw bytes. GNU tools on
# the Linux runner are unaffected either way.
export LC_ALL=C

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

# Apps driven with an injected user-button press (board_sim --button 1 holds
# SW1/P009 low). gpio_input_demo mirrors SW1 -> LED1, so with the button held
# its LED1 must read ON -- this gates the GPIO input-injection path (#39).
button_apps="gpio_input_demo"

# Apps driven with an injected panel tap (board_sim --click X Y arms one GT911
# contact, re-armed each chunk until the firmware's real ra_touch_read drains
# it). touch_demo brings up the GT911 and decodes that tap, so the injected
# coordinate must come back in its banner -- this gates the GT911 touch path
# end to end (#122). 250,250 maps 1:1 on the default panel (no rotation).
touch_click_apps="touch_demo"

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
# tz_secure_only_sd does a write+read+compare roundtrip, so it needs the card
# (board_sim's SD model now answers CMD24/CMD25 block writes into the image).
sd_apps="sd_font_render tz_secure_only_sd"
sd_image=""

# USB device-enumeration apps (#67 Phase 3 -- the headline USB-debugging goal).
# board_sim's virtual USB host (board_usb.c) watches SYSCFG.DPRPU and drives the
# real chapter-9 SETUP sequence (GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION
# plus the class-specific traffic) against the firmware's actual USBX device stack
# + USBFS DCD register model. For these apps we assert the device reaches
# CONFIGURED with its class active -- i.e. enumeration completed end to end, with
# no hardware: CDC-ACM, HID (boot mouse), and MSC (BOT/SCSI + a sector read).
# They are ThreadX/USBX, so (like the LevelX/FileX apps) they need a newer Unicorn
# than the CI runner's 2.0.1 and a bounded budget; pass them explicitly, e.g.
# `scripts/board_sim_smoke.sh usb_cdc_echo usb_msc_device`.
usb_enum_apps="usb_cdc_echo threadx_usbx_cdc_demo usb_hid_device usb_msc_device"

# USB HOST-mode apps (#67 Phase 3, the inverse path). board_sim seams the
# first-party ra_usb_host_* primitives to a virtual HID boot keyboard (the same
# function-seam technique it uses for ra_eth_*), since the USBHS host controller
# (0x40351000) is unmodelled. The firmware's real host stack enumerates the
# virtual device, opens the interrupt-IN pipe, and decodes its reports -- we
# assert the app's end-to-end PASS banner. ThreadX, so newer-Unicorn-only; pass
# explicitly, e.g. `scripts/board_sim_smoke.sh usb_host_keyboard`.
usb_host_apps="usb_host_keyboard usb_host_msc_browse usb_host_file_ops"

# microSD FORMAT apps: exercise the ra_fs_format() mkfs path end to end. These
# reformat the modelled card themselves (once per FAT type), so they take a
# blank card via board_sim's --sd-new (not a pre-built --sd image). board_sim's
# SD model answers the block writes the formatter and ra_fs emit, and its CSD
# reports the --sd-new size so the SD bring-up sees a real capacity.
# fs_format_mount formats + mounts + file-cycles FAT12, FAT16, FAT32 (+ an
# exFAT format + mount + empty-root trial) and
# prints "FS FORMAT+MOUNT ALL PASS". Bare-metal (no ThreadX), so it runs on the
# CI runner's Unicorn too, but it is opt-in like the other card apps: pass it
# explicitly, e.g. `scripts/board_sim_smoke.sh fs_format_mount`.
sd_format_apps="fs_format_mount"

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
    case " $button_apps " in
    *" $1 "*) printf -- '--button 1' ;;
    esac
    case " $touch_click_apps " in
    *" $1 "*) printf -- '--click 250 250' ;;
    esac
    # bkup_survival_demo proves reset-survival: --reboot makes the sim re-run the
    # firmware from its reset vector with the VBATT backup domain retained, so the
    # second boot finds the sentinel and reports survived=Y.
    case "$1" in
    bkup_survival_demo) printf -- '--reboot 1' ;;
    esac
}

# Echo the UART substring an app must print for its peripheral to count as
# "meaningfully exercised" (board_sim emits SCI TX as `[uart] SCIn: ...`), or
# nothing for apps with no such banner (display / button / render apps). This
# upgrades the gate from "booted without faulting" to "produced the right
# peripheral data" -- e.g. crc_demo's hw CRC must equal its sw CRC, dma_memcopy
# must actually copy, adc must read midscale, the RTC alarm must fire.
uart_expect() { # app -> expected UART substring on stdout
    case "$1" in
    uart_hello)         printf 'hello, ra8d2!' ;;
    ereader_chrome_hil) printf 'ereader-hil: chrome boxes=7 crc=0DCB740F' ;;
    ereader_image_hil)  printf 'ereader-img-hil: img 160x120 crc=BDC56EC5' ;;
    ereader_link_hil)   printf 'ereader-link-hil: links=2 cross=Y frag=Y apage=1 geom=5B90D1EE' ;;
    ereader_align_hil)  printf 'ereader-align-hil: glyphs=210 geom=D4C9657E' ;;
    ereader_table_hil)  printf 'ereader-table-hil: glyphs=172 geom=E3181EE6' ;;
    reflow_content_hil) printf 'reflow-content-hil: pages=14 crc=D211DBC5 rpages=33 crc=62C68DC5' ;;
    ereader_input_hil)  printf 'ui-hil: taps=7 hits=5 nav_ok=1 PASS' ;;
    ereader_cover_hil)  printf 'ereader-cover-hil: cover 80x120 crc=6E4E45C5 PASS' ;;
    ereader_svg_hil)    printf 'ereader-svg-hil: svg 100x100 crc=A6450BE6 PASS' ;;
    ereader_imgfmt_hil) printf 'ereader-imgfmt-hil: bmp=D53617C5 gif=350551C5 PASS' ;;
    ereader_jpeg_hil)   printf 'ereader-jpeg-hil: img 160x120 crc=F71D21E8' ;;
    epub_parse_hil)     printf 'epub: chapters=2 ch0_crc=CF23AEEE PASS' ;;
    epub_stress_hil)    printf 'epub-stress-hil: files=125 chapters=60 toc=60 cover=ok PASS' ;;
    widget_app_hil)     printf 'widget-app-hil: apps=2 lib=D3FB85C5 rdr=E9E475C5 flush=160x16 hint=fast PASS' ;;
    glcdc_render_hil)   printf 'glcdc-hil: layer1=ok dim=512x512 crc=B21B8D3D PASS' ;;
    bscan_selftest)     printf 'bscan: idcode=085DA447 checks=17 PASS' ;;
    keyboard_hil)       printf 'kbd: q=Hi 9 commit=1 taps=7 PASS' ;;
    touch_demo)         printf 'touch: open=OK pts=1 x=250 y=250' ;;
    smbus_demo)         printf 'smbus: whoami=6C sendrecv=6C PASS' ;;
    battery_monitor_demo) printf 'battery: soc=72%% chg=N PASS' ;;
    crc_demo)           printf 'match=Y' ;;
    adc_b_demo)         printf 'adc: raw=' ;;
    agt_periodic)       printf 'agt: tick' ;;
    i2c_loopback)       printf 'i2c: scan 0x43 ack=1' ;;
    eth_loopback)       printf 'etha: loopback ok' ;;
    crypto_aes_demo)    printf 'aes: round-trip OK' ;;
    dma_memcopy_demo)   printf 'dma: copied 1024B match=Y' ;;
    rtc_alarm)          printf 'rtc: alarm fired' ;;
    elc_event_demo)     printf 'elc: en=1 trig=' ;;
    timer_capture_demo) printf 'gpt: period=' ;;
    drw_fill_demo)      printf 'drw: fill match=Y' ;;
    dtc_transfer_demo)  printf 'dtc: copied 1024B match=Y' ;;
    cac_accuracy_demo)  printf 'cac: meas=ok ferr=0 ovf=0 ok=Y' ;;
    lvd_monitor_demo)   printf 'lvd: pvd1 thr=2.80V mon=above det=0 ok=Y' ;;
    pdg_delay_demo)     printf 'pdg: dll=on ch0=on delay=0x40 cfg=ok' ;;
    dotf_selftest_demo) printf 'dotf: ch0/1 init=ok selftest=run ok=Y' ;;
    ecc_monitor_demo)   printf 'ecc: sram2 ecc=on rw=ok ok=Y' ;;
    bkup_survival_demo) printf 'bkup: rw=ok survived=Y' ;;
    wdt_reset_recovery_demo) printf 'wdt: reset_by=watchdog' ;;
    lpm_idle_demo)      printf 'lpm: wake_count=' ;;
    lpm_deep_sleep_demo) printf 'lpm_deep: woke' ;;
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
        ereader_chrome_hil ereader_image_hil ereader_link_hil ereader_align_hil ereader_table_hil \
        reflow_content_hil ereader_input_hil bscan_selftest keyboard_hil touch_demo \
        uart_hello gpt_irq_demo ssie_audio_loop crc_demo doc_demo \
        canfd_loopback imu_lsm6dso_demo smbus_demo battery_monitor_demo gpio_input_demo \
        adc_b_demo agt_periodic dma_memcopy_demo rtc_alarm elc_event_demo \
        timer_capture_demo drw_fill_demo dtc_transfer_demo \
        cac_accuracy_demo lvd_monitor_demo pdg_delay_demo \
        dotf_selftest_demo ecc_monitor_demo \
        bkup_survival_demo reset_cause_demo wdt_reset_recovery_demo \
        lpm_idle_demo lpm_deep_sleep_demo \
        ereader_cover_hil ereader_svg_hil ereader_imgfmt_hil ereader_jpeg_hil \
        epub_parse_hil epub_stress_hil widget_app_hil glcdc_render_hil \
        acmphs_compare can_classic_loopback canfd_filter_demo dac_b_demo dac_waveform \
        gpt_capture_input gpt_dma_demo gpt_one_shot_demo gpt_pwm_demo gpt_three_phase_demo \
        i2c_loopback flash_journal eth_loopback clock_check crypto_aes_demo)
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
    # USB device-enumeration apps: the virtual host drives chapter-9; assert the
    # device reaches CONFIGURED. BOARD_SIM_USB_STOP stops the run a short settle
    # window after enumeration completes -- these apps never idle (HID jiggles its
    # mouse, MSC keeps answering polls), so this is what keeps the gate fast and
    # deterministic. MAX_CHUNKS is the failure cap (no CONFIGURED -> run ends ->
    # ENUM FAIL); the WALL_S floor stops the wall-clock guard truncating first.
    case " $usb_enum_apps " in
    *" $app "*)
        uout="$(BOARD_SIM_MAX_CHUNKS=8000 BOARD_SIM_USB_STOP=300 BOARD_SIM_WALL_S=300 \
            "$sim" "$elf" 2>&1 || true)"
        if echo "$uout" | grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT"; then
            echo "FAULT (during USB bring-up)"
            fail=1
        elif echo "$uout" | grep -q "device CONFIGURED"; then
            klass="$(echo "$uout" | sed -n 's/.*device CONFIGURED (\(.*\)).*/\1/p' | head -1)"
            echo "OK (USB enumerated -> CONFIGURED, $klass)"
        else
            echo "USB ENUM FAIL (device did not reach CONFIGURED)"
            fail=1
        fi
        continue
        ;;
    esac
    # USB host-mode apps: the firmware's host stack enumerates board_sim's virtual
    # device and drives it end to end. Three classes are covered: a HID boot
    # keyboard (reports decode to "RA8D2"), a read-only FAT16 MSC disk (mount +
    # browse + content-verify 1 MiB vs MRAM + write-reject), and a read-WRITE
    # FAT16 disk (usb_host_file_ops: create / read-back / rename / unlink a file).
    # BOARD_SIM_STOP_ON stops the run as soon as the success line prints (these
    # apps loop forever otherwise). Assert the app's own PASS banner.
    case " $usb_host_apps " in
    *" $app "*)
        hout="$(BOARD_SIM_MAX_CHUNKS=20000 BOARD_SIM_STOP_ON='PASS' BOARD_SIM_WALL_S=300 \
            "$sim" "$elf" 2>&1 || true)"
        if echo "$hout" | grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT"; then
            echo "FAULT (during USB host bring-up)"
            fail=1
        elif echo "$hout" | grep -qE "USB HOST (KEYBOARD|MSC BROWSE) PASS|ALL FILE OPS PASSED"; then
            banner="$(echo "$hout" | grep -oE "USB HOST [A-Z ]*PASS|ALL FILE OPS PASSED" | head -1)"
            echo "OK ($banner)"
        else
            echo "USB HOST FAIL (host did not reach its PASS banner)"
            fail=1
        fi
        continue
        ;;
    esac
    # microSD FORMAT apps: attach a blank 64 MiB card with --sd-new (the app
    # reformats it as FAT12/FAT16/FAT32 in turn), then assert the app's own
    # PASS banner. 64 MiB is large enough that the formatter's auto cluster-size
    # sweep lands every type in its valid band. The app loops forever after the
    # banner, so BOARD_SIM_STOP_ON ends the run as soon as ALL PASS prints.
    case " $sd_format_apps " in
    *" $app "*)
        # The SD apps interleave SPI-bus noise -- including NUL bytes -- on the
        # modelled SCI8 TX, which bash command-substitution mangles (it drops
        # NULs and can split the banner). Strip non-printables at the pipe,
        # before bash captures the text, so the ASCII banner survives intact.
        # The SCI8 "TX <n> bytes" total printed by board_sim is the independent
        # completeness signal; this grep is the readable assertion.
        fclean="$({ BOARD_SIM_MAX_CHUNKS=40000 BOARD_SIM_STOP_ON='ALL PASS' BOARD_SIM_WALL_S=300 \
            "$sim" "$elf" --sd-new 64:fat32 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
        # Match with here-strings, not `echo ... | grep -q`: the run log is large
        # and `grep -q` closes the pipe on first match, so under `pipefail` the
        # upstream `echo` dies with SIGPIPE and fails the whole compound command
        # (a false negative). A here-string has no pipe to break.
        if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$fclean"; then
            echo "FAULT (during format/mount)"
            fail=1
        elif grep -q "FS FORMAT+MOUNT ALL PASS" <<<"$fclean"; then
            echo "OK (FS FORMAT+MOUNT ALL PASS -- FAT12/16/32 + exFAT)"
        else
            echo "FS FORMAT FAIL (did not reach ALL PASS banner)"
            fail=1
        fi
        continue
        ;;
    esac
    out="$("$sim" "$elf" $extra 2>&1 || true)"
    if echo "$out" | grep -q "INVALID INSN\|UNMAPPED"; then
        echo "FAULT (invalid opcode / unmapped access)"
        fail=1
        continue
    fi
    # A firmware BKPT (Default_Handler's bkpt, a failed assert, a fault give-up)
    # is a halt regardless of which symbol it sits in -- board_sim stops on it
    # and prints this line. Catch it directly so a bare assert outside the named
    # *panic_halt / *_halt_loop symbols pc_in_halt_loop() knows about still fails.
    if echo "$out" | grep -q "executed a BKPT"; then
        echo "BKPT HALT ($(echo "$out" | sed -n 's/.*executed a BKPT @ *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1))"
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
    case " $button_apps " in
    *" $app "*)
        # The app ran with --button held (see sim_extra_args). gpio_input_demo
        # mirrors SW1 -> LED1, so the injected press must light LED1.
        if echo "$out" | grep -qE "LED1[^]]*ON"; then
            echo "OK (pc=$pc, --button -> LED1 ON)"
        else
            echo "BUTTON NO-OP (pc=$pc; --button did not light LED1)"
            fail=1
        fi
        continue
        ;;
    esac
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
    # Peripheral apps: assert the app actually produced its real SCI output, not
    # just that it reached the run budget (the #67 "exercise it meaningfully" bar).
    want="$(uart_expect "$app")"
    if [ -n "$want" ]; then
        if echo "$out" | grep -qF "$want"; then
            echo "OK (pc=$pc, uart: '$want')"
        else
            echo "UART MISMATCH (pc=$pc; expected '$want' in the SCI output)"
            fail=1
        fi
        continue
    fi
    echo "OK (pc=$pc)"
done

[ -n "$sd_image" ] && rm -f "$sd_image"

# On-screen SW1 button (#39 interactive --view input layer): a click on the
# sidebar's SW1 push-button must route to the user-switch model (drive P009 low),
# NOT the touch panel -- so gpio_input_demo lights LED1 (SW1 -> LED1) while
# draining zero GT911 touches. The SW1 button face centre sits at composite
# (1117,442) on the default 1024x600 panel (panel_w 1024 + k_btn_x_dx 18 + half
# of k_btn_w 150 = 1117; k_btn_y 424 + half of k_btn_h 36 = 442); this gates
# board_overlay_hit_button + route_click. Only when gpio_input_demo is built.
case " ${apps[*]} " in
*" gpio_input_demo "*)
    printf '  %-24s ' "on-screen SW1 click"
    gelf="$(find examples -path '*/gpio_input_demo/build/gpio_input_demo.elf' 2>/dev/null | head -1)"
    bout="$("$sim" "$gelf" --click 1117 442 2>&1 || true)"
    if echo "$bout" | grep -qE "LED1[^]]*ON" && echo "$bout" | grep -qE "touch clicks  : 0 "; then
        echo "OK (sidebar SW1 -> P009 -> LED1 ON, 0 touches)"
    else
        echo "BUTTON CLICK NO-OP (on-screen SW1 did not light LED1 via GPIO)"
        fail=1
    fi

    # Keyboard path (#39): --keys pushes a scripted string through the SAME
    # board_input FIFO the live --view window's keyDown feeds; the run loop drains
    # it to the console UART RX. uart_irq_echo echoes RX back, so the typed marker
    # must reappear on its UART line -- a headless, deterministic test of the
    # keyboard input with no window and no OS key events (gates board_input +
    # the run-loop key drain).
    printf '  %-24s ' "--keys -> UART echo"
    if make uart_irq_echo >/tmp/smoke_build_uart_irq_echo.log 2>&1; then
        kelf="$(find examples -path '*/uart_irq_echo/build/uart_irq_echo.elf' 2>/dev/null | head -1)"
        kout="$(BOARD_SIM_IDLE_STOP=6000 "$sim" "$kelf" --keys 'KBDSMOKE\r\n' 2>&1 || true)"
        if echo "$kout" | grep -qE "\[uart\] SCI8: KBDSMOKE"; then
            echo "OK (--keys -> board_input -> SCI8 RX -> echo)"
        else
            echo "KEYS NO-OP (--keys did not reach the UART echo)"
            fail=1
        fi
    else
        echo "BUILD FAIL (see /tmp/smoke_build_uart_irq_echo.log)"
        fail=1
    fi
    ;;
esac

# Low-battery nag (ra_batt policy): seed the modelled fuel gauge below each
# threshold via --battery and assert the edge-triggered warning line. 15% must
# raise LOW (<=20), 8% must raise CRITICAL (<=10); the default 72% run gated
# above already proves no nag fires when healthy. Only when the app is built.
case " ${apps[*]} " in
*" battery_monitor_demo "*)
    belf="$(find examples -path '*/battery_monitor_demo/build/battery_monitor_demo.elf' 2>/dev/null | head -1)"
    printf '  %-24s ' "battery nag LOW"
    lout="$(BOARD_SIM_STOP_ON='NAG' BOARD_SIM_WALL_S=20 BOARD_SIM_MAX_CHUNKS=6000 \
        "$sim" "$belf" --battery 15 2>&1 || true)"
    if echo "$lout" | grep -q "NAG LOW soc=15%"; then
        echo "OK (--battery 15 -> NAG LOW)"
    else
        echo "NAG FAIL (15% did not raise LOW)"
        fail=1
    fi
    printf '  %-24s ' "battery nag CRITICAL"
    cout="$(BOARD_SIM_STOP_ON='NAG' BOARD_SIM_WALL_S=20 BOARD_SIM_MAX_CHUNKS=6000 \
        "$sim" "$belf" --battery 8 2>&1 || true)"
    if echo "$cout" | grep -q "NAG CRITICAL soc=8%"; then
        echo "OK (--battery 8 -> NAG CRITICAL)"
    else
        echo "NAG FAIL (8% did not raise CRITICAL)"
        fail=1
    fi
    ;;
esac

if [ "$fail" -eq 0 ]; then
    echo "board_sim smoke: ALL PASS"
    exit 0
fi
echo "board_sim smoke: FAILURES"
exit 1
