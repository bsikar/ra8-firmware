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

# UI apps whose rendered frame must be rich (distinct-color floor). ereader_ui
# is the e-reader chrome (ra8_box + ra8_gfx); this gates that it actually paints.
# sd_font_render reads FONT.OTF off the modelled microSD and reflows it, so its
# frame must also be non-trivial.
render_assert_apps="ereader_ui sd_font_render"
min_render_colors=6

# Apps driven with an injected user-button press (board_sim --button 1 holds
# SW1/P009 low). gpio_input_demo mirrors SW1 -> LED1, so with the button held
# its LED1 must read ON -- this gates the GPIO input-injection path (#39).
button_apps="gpio_input_demo"

# Apps driven with an injected panel tap (board_sim --click X Y arms one GT911
# contact, re-armed each chunk until the firmware's real ra8_touch_read drains
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

# Apps whose UART banner is emitted by a free-running timer poll (the AGT/RTC/ELC
# tick demos). The emulation is instruction-deterministic, but board_sim's
# wall-clock guard is CPU-time (clock()), so on a heavily-loaded runner Unicorn's
# TCG re-translation burned the guard's budget faster than wall time and
# TRUNCATED the run before the (deterministic, but a-few-chunks-in) banner
# printed -- and a truncated run used to be mislabelled "EXECUTED to the run
# budget", so the gate accepted it banner-less. That is the whole #168 flake: not
# emulator non-determinism, a CPU-time guard plus a mislabel. Fixed three ways:
# board_sim now reports a truncation honestly (a TRUNCATED line that fails the
# budget check below instead of masquerading as success); BOARD_SIM_WALL_S=0 now
# truly DISABLES that CPU-time guard; and these apps run with WALL_S=0 so they are
# bounded ONLY by the deterministic instruction-counted chunk budget, never by
# host CPU-time -- so the banner is emitted on every run regardless of load.
# BOARD_SIM_STOP_ON ends each run at the banner so disabling the guard cannot make
# a genuinely-stuck app run long. The banner is a hard assertion again (no
# WARN-accept); a small retry stays as cheap insurance only.
periodic_tick_apps="agt_periodic rtc_alarm elc_event_demo"

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
#
# usb_printer_vendor (issue #265) is the odd one out: it is bare-metal (no
# ThreadX/USBX) and answers the same chapter-9 script from a hand-rolled polled
# responder that drives the native ra8_usb_pprn (Printer 7/1/2) + ra8_usb_pvnd
# (Vendor 0xFF) class layers. The virtual host detects its first interface
# (Printer 0x07) and the run reaches "device CONFIGURED (Printer active)"; the
# same CONFIGURED assertion applies.
usb_enum_apps="usb_cdc_echo threadx_usbx_cdc_demo usb_hid_device usb_msc_device usb_printer_vendor"

# USB HOST-mode apps (#67 Phase 3, the inverse path). board_sim seams the
# first-party ra8_usb_host_* primitives to a virtual HID boot keyboard (the same
# function-seam technique it uses for ra8_eth_*), since the USBHS host controller
# (0x40351000) is unmodelled. The firmware's real host stack enumerates the
# virtual device, opens the interrupt-IN pipe, and decodes its reports -- we
# assert the app's end-to-end PASS banner. ThreadX, so newer-Unicorn-only; pass
# explicitly, e.g. `scripts/board_sim_smoke.sh usb_host_keyboard`.
usb_host_apps="usb_host_keyboard usb_host_msc_browse usb_host_file_ops"

# Live-SD USB MSC device app (#206): the MSC LUN serves the ACTUAL modelled
# card (--sd), not a snapshot -- media-read/media-write forward to
# ra8_sdmmc_spi_read_blocks / ra8_sdmmc_spi_write_blocks and the LUN geometry is
# the card's CSD capacity. board_sim's virtual USB host enumerates the device
# and drives the MSC BOT script (INQUIRY, READ CAPACITY(10), READ(10) of
# sector 0), so the gate asserts CONFIGURED (MSC active) PLUS the capacity
# reported over the USB pipe equals the card image's real block count PLUS a
# full 512-byte sector-0 read -- proving capacity + data flow card ->
# ra8_sdmmc_spi -> media callbacks -> USB end to end. The scripted host issues
# no WRITE(10); the real-PC file copy stays a manual bench step (README.md).
# ThreadX/USBX, so newer-Unicorn-only; pass explicitly, e.g.
# `scripts/board_sim_smoke.sh usb_msc_sdcard`.
usb_msc_sd_apps="usb_msc_sdcard"

# microSD FORMAT apps: exercise the ra8_fs_format() mkfs path end to end. These
# reformat the modelled card themselves (once per FAT type), so they take a
# blank card via board_sim's --sd-new (not a pre-built --sd image). board_sim's
# SD model answers the block writes the formatter and ra8_fs emit, and its CSD
# reports the --sd-new size so the SD bring-up sees a real capacity.
# fs_format_mount formats + mounts + file-cycles FAT12, FAT16, FAT32 (+ an
# exFAT format + mount + empty-root trial) and
# prints "FS FORMAT+MOUNT ALL PASS". Bare-metal (no ThreadX), so it runs on the
# CI runner's Unicorn too, but it is opt-in like the other card apps: pass it
# explicitly, e.g. `scripts/board_sim_smoke.sh fs_format_mount`.
sd_format_apps="fs_format_mount"

# microSD ra8_io apps: prove the ra8_io fabric's swappable SD-over-SPI block-device
# backend (ra8_io_blockdev_sdspi) by formatting + mounting FAT16 on a blank
# --sd-new card and round-tripping a file through the VFS. Distinct from the
# fs_format_mount banner, so it gets its own banner assertion via uart_expect().
sd_io_apps="ra8_io_sd_demo ra8_io_sdhi_demo ra8_sdhi_card_demo"

# On-chip non-volatile ra8_io apps (no CLI flag -- board_sim models the medium
# internally): OSPI NOR (ra8_io_xspi_demo, erase-before-write 4 KiB RMW) and the
# on-chip extra MRAM (ra8_io_mram_demo, MACI program/erase via board_periph_mram).
# Both idle forever after their PASS banner, so STOP_ON ends the run the moment
# it prints. Asserts via uart_expect().
xspi_io_apps="ra8_io_xspi_demo ra8_io_mram_demo"

# IT8951 e-paper apps (#256): board_sim attaches a modelled IT8951 controller on
# SPI_B with --eink (board_periph_eink.c), which answers HRDY, the GET_DEV_INFO
# drain and the LUTAFSR "LUT idle" poll, so epaper_refresh drives the full
# ra8_epaper -> display-PAL e-ink -> ra8_io_spi_bus path to its PASS banner with
# no panel. The per-pixel SPI streaming (full + partial refresh) needs a bigger
# chunk budget than the default; STOP_ON ends the run at the banner. Without
# --eink HRDY never asserts and the app honestly reports FAIL, so the model is
# load-bearing (SIM == HIL). Asserts via uart_expect().
eink_apps="epaper_refresh"

# Cellular AT-modem app (#259): board_sim attaches a modelled 3GPP AT modem on
# the MikroBUS UART (SCI7) with --modem (board_periph_modem.c), which answers
# the demo's AT script -- AT / ATE0 / CMEE / CPIN? / CSQ / CREG=1 (+ a +CREG
# URC) / CREG? / CGATT? -- and rejects an unsupported command with +CME ERROR,
# so modem_at_demo drives the full ra8_modem_at -> ra8_sci path (sync, SIM,
# signal, registration, PS attach, error path) to its PASS banner with no
# physical modem. Without --modem the modem never answers and the app honestly
# reports "modem: sync FAIL", so the model is load-bearing (SIM == HIL for the
# AT protocol). Asserts via uart_expect().
modem_apps="modem_at_demo"

# Deep-idle self-parking apps. lpm_periodic_idle runs a bounded wake/work/standby
# loop, prints "lpm_periodic_idle PASS", then parks forever in lpi_panic_halt --
# a deliberate low-power WFI spin (the same success-park its bench-proven sibling
# lpm_ulpt_standby uses). Because that park symbol's name ends in *panic_halt, the
# standard runner's pc_in_halt_loop() would misread the success-park as a fault
# give-up. So, exactly like the "print PASS then idle" SD/OSPI apps above, stop
# the run at the banner and assert it directly -- but with the NORMAL 300 s wall
# guard and a single run, NOT the periodic-tick WALL_S=0 path (that path is the
# #168 deterministic fix for agt_periodic/rtc_alarm/elc_event_demo and is left
# untouched). Asserts via uart_expect().
selfpark_banner_apps="lpm_periodic_idle"

# Dual-core ITM-verdict apps (#67 / #152). Unlike the UART-banner apps, these
# narrate their run over ra8_log, which the firmware writes to the Cortex-M ITM
# stimulus port; board_sim surfaces those bytes as `[itm] ...` lines (the SWO
# analog of the `[uart]` console echo). The M85 narrates the M33's replies too
# -- a dualcore_mailbox reply of operand*3+1, or the background counter's final
# total, can only come from the SECOND core actually executing -- so the ITM
# verdict gates the dual-core release + cross-core shared-SRAM IPC path end to
# end, with no hardware. They are built Debug (see the build loop) because
# ra8_log_info is compiled to a no-op at the default RelWithDebInfo level, and run
# with BOARD_SIM_STOP_ON tied to each app's PERSISTENT steady-state ITM line so
# the run ends deterministically the moment the verdict has been logged (the M85
# parks / heartbeats forever afterwards). Bare-metal (no ThreadX), and the
# already-gated compile_on_m33 proves the CI runner's Unicorn spins up the second
# (M33) engine, so these run in the default list too.
dualcore_itm_apps="dualcore_background_m33 dualcore_mailbox"

# Persistent `[itm]` substring BOARD_SIM_STOP_ON waits for: the M85 keeps logging
# it forever after the verdict, so it is the current last console line whenever
# the chunk-boundary stop check runs (a transient summary line that scrolled past
# within one chunk would be missed).
dualcore_stop() { # app -> persistent ITM stop substring on stdout
  case "$1" in
    dualcore_background_m33) printf 'dualcore_background_m33 PASS' ;;
    dualcore_mailbox) printf 'heartbeat: both cores alive' ;;
  esac
}

# Strong `[itm]` verdict asserted in the captured run output -- only producible
# by the M33 (second core) having executed (the background PASS prints only when
# the M33's counter hit its target; the mailbox total counts the M33's replies).
dualcore_assert() { # app -> ITM verdict substring to assert on stdout
  case "$1" in
    dualcore_background_m33) printf 'dualcore_background_m33 PASS' ;;
    dualcore_mailbox) printf 'demo done; total M33 replies=6' ;;
  esac
}

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

# Apps that import a `.epub` off the SD card and compile it to a cached `.rabook`
# on first open (#151). import_reader mounts the card, finds BOOK.EPB, compiles
# it once (cache MISS), reopens it (cache HIT, no recompile), then reads the
# cached book back -- printing "import_reader: miss->compile->cache->hit->read
# PASS". A BLANK card has no BOOK.EPB, so it fails at the source open; this gate
# bakes a tiny deterministic EPUB onto the card (build_book_sd_image) so the full
# import + compile + cache + read path runs end to end in board_sim with no
# hardware. The seed book carries a `text/css` stylesheet (style.css) so the run
# exercises the runtime ra8_rabook stylesheet-compile stage end to end, not just
# text (#169). This was previously gated text-only on the belief it tripped a
# board_sim emulation gap; the real cause was a firmware bug -- import_reader's
# pipeline scratch never wired a `.css` source buffer (css_cap == 0), so any
# `text/css` item failed the compile on every target, not just in the emulator.
# With the buffer wired, the full importer runs (mount -> CRC key ->
# compile-to-blob incl. stylesheets -> temp+rename cache -> reopen hit ->
# read-back validate).
import_reader_apps="import_reader"
book_sd_image=""

# Build a microSD card image carrying the import_reader text EPUB as BOOK.EPB.
# Packs the in-repo fixture component dir into a byte-deterministic `.epub`
# (mimetype first + every member STORED with a fixed 1980 epoch, so the bytes
# never drift across Python/zlib versions or run times -- the same recipe
# scripts/utils/rabook_parity_gen.py uses), then writes it through mkfontimg's
# real ra8_fs so the on-card FAT layout is exactly what the firmware reads. Sets
# the global $book_sd_image on success; leaves it empty if the fixture, Python,
# or mkfontimg is unavailable (the gate then fails loudly, as a missing card is a
# real setup error for this app).
build_book_sd_image() {
  local fixture="$ROOT/examples/ek_ra8d2/hw_pending/import_reader/fixtures/book_src"
  local mk="$ROOT/tools/mkfontimg"
  local epub
  [ -d "$fixture" ] || return 0
  cmake -B "$mk/build" -S "$mk" >/dev/null 2>&1 || return 0
  cmake --build "$mk/build" >/dev/null 2>&1 || return 0
  epub="$(mktemp -t board_sim_book.XXXXXX.epub)"
  if ! python3 - "$fixture" "$epub" <<'PY'; then
import sys, zipfile
from pathlib import Path

src, out = Path(sys.argv[1]), sys.argv[2]
epoch = (1980, 1, 1, 0, 0, 0)  # fixed timestamp -> byte-deterministic .epub
with zipfile.ZipFile(out, "w") as zf:
    mt = zipfile.ZipInfo("mimetype", epoch)
    mt.compress_type = zipfile.ZIP_STORED
    zf.writestr(mt, b"application/epub+zip")
    for path in sorted(p for p in src.rglob("*") if p.is_file()):
        info = zipfile.ZipInfo(path.relative_to(src).as_posix(), epoch)
        info.compress_type = zipfile.ZIP_STORED
        zf.writestr(info, path.read_bytes())
PY
    rm -f "$epub"
    return 0
  fi
  book_sd_image="$(mktemp -t board_sim_book.XXXXXX.img)"
  "$mk/build/mkfontimg" "$epub" "$book_sd_image" BOOK.EPB >/dev/null 2>&1 || book_sd_image=""
  rm -f "$epub"
}

msc_sd_image=""

# Build the full-capacity card image for the live-SD USB MSC gate. mkbookimg
# always emits a 64 MiB FAT32 image (131072 x 512-B sectors -- an exact
# 512 KiB multiple, so board_sim's CSD synthesis reports precisely that block
# count and the capacity assertion is byte-exact). The baked file just makes
# the volume non-empty; LICENSE.txt is always present in the repo. Sets the
# global $msc_sd_image on success; leaves it empty if mkbookimg is
# unavailable (the gate then fails loudly -- the card IS the test medium).
build_msc_sd_image() {
  local mk="$ROOT/tools/mkbookimg"
  cmake -B "$mk/build" -S "$mk" >/dev/null 2>&1 || return 0
  cmake --build "$mk/build" >/dev/null 2>&1 || return 0
  msc_sd_image="$(mktemp -t board_sim_msc_sd.XXXXXX.img)"
  "$mk/build/mkbookimg" "$msc_sd_image" "$ROOT/LICENSE.txt" >/dev/null 2>&1 || msc_sd_image=""
}

# I2C/I3C TARGET-mode apps. i3c_i2c_peripheral_demo brings IIC_B up as an
# addressed peripheral (it programmes its own MSDVAD address and waits to be
# addressed) and emits NO UART -- the only on-chip signal is LED1, which toggles
# on every accepted transaction. board_sim's I2C model now plays the EXTERNAL
# controller (board_periph_i2c.c i3c_periph_*): it writes a byte the firmware
# drains, then reads the firmware's one-byte echo, so the responder driver runs
# end to end with no wired-up bus. board_sim prints a target summary line at run
# end ("I3C/I2C target: ... echo=Y"); the run is bounded by a small chunk budget
# (the demo idles forever otherwise) and the echo verdict is deterministic the
# moment any transaction completes.
i3c_target_apps="i3c_i2c_peripheral_demo"

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
    uart_hello) printf 'hello, ra8d2!' ;;
    ra8_io_demo) printf 'ra8_io_demo: mkdir+nested ram:/SUB/NOTE.TXT PASS' ;;
    ra8_io_sdram_demo) printf 'ra8_io_sdram_demo: mkdir+nested dr:/SUB/NOTE.TXT PASS' ;;
    ra8_io_compress_demo) printf 'bytes -> 4096 round-trip PASS' ;;
    ra8_io_sd_demo) printf 'ra8_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS' ;;
    ra8_io_sdhi_demo) printf 'ra8_io_sdhi_demo: sd:/LOGS/A.TXT 512 bytes PASS' ;;
    ra8_sdhi_card_demo) printf 'ra8_sdhi_card_demo: native SDHI block round-trip PASS' ;;
    ra8_io_xspi_demo) printf 'ra8_io_xspi_demo: xs:/CFG/SET.BIN 256 bytes PASS' ;;
    ra8_io_mram_demo) printf 'block erase/program/read on extra MRAM PASS' ;;
    epaper_refresh) printf 'epaper: PASS' ;;
    modem_at_demo) printf 'modem: rssi=17 reg=1 attach=1 cme=ok PASS' ;;
    ra8_io_fsfmt_demo) printf 'ra8_io_fsfmt_demo: probed fat maxname=12 + foreign stub seam PASS' ;;
    ra8_io_cache_demo) printf 'ra8_io_cache_demo: re-read x8 hits=' ;;
    ereader_chrome) printf 'ereader-hil: chrome boxes=7 crc=0DCB740F' ;;
    ereader_image) printf 'ereader-img-hil: img 160x120 crc=BDC56EC5' ;;
    ereader_link) printf 'ereader-link-hil: links=2 cross=Y frag=Y apage=1 geom=5B90D1EE' ;;
    ereader_align) printf 'ereader-align-hil: glyphs=210 geom=D4C9657E' ;;
    ereader_table) printf 'ereader-table-hil: glyphs=172 geom=E3181EE6' ;;
    reflow_content) printf 'reflow-content-hil: pages=14 crc=D211DBC5 rpages=33 crc=62C68DC5' ;;
    ereader_input) printf 'ui-hil: taps=7 hits=5 nav_ok=1 PASS' ;;
    ereader_cover) printf 'ereader-cover-hil: cover 80x120 crc=6E4E45C5 PASS' ;;
    ereader_svg) printf 'ereader-svg-hil: svg 100x100 crc=A6450BE6 PASS' ;;
    ereader_imgfmt) printf 'ereader-imgfmt-hil: bmp=D53617C5 gif=350551C5 PASS' ;;
    ereader_jpeg) printf 'ereader-jpeg-hil: img 160x120 crc=F71D21E8' ;;
    ereader_longstrip) printf 'ereader-longstrip: bands=16 view=1024x600 scroll=0 crc=795D27E6' ;;
    epub_parse) printf 'epub: chapters=2 ch0_crc=CF23AEEE PASS' ;;
    epub_stress) printf 'epub-stress-hil: files=125 chapters=60 toc=60 cover=ok PASS' ;;
    widget_app) printf 'widget-app-hil: apps=2 lib=D3FB85C5 rdr=E9E475C5 flush=160x16 hint=fast PASS' ;;
    widget_app_demo) printf 'widget-app-demo: apps=3 lib=26CE7CD0 rdr=22B7E671 route=ok flush=512x44 hint=fast PASS' ;;
    glcdc_render) printf 'glcdc-hil: layer1=ok dim=512x512 crc=B21B8D3D PASS' ;;
    bscan_selftest) printf 'bscan: idcode=085DA447 checks=17 PASS' ;;
    keyboard) printf 'kbd: q=Hi 9 commit=1 taps=7 PASS' ;;
    touch_demo) printf 'touch: open=OK pts=1 x=250 y=250' ;;
    smbus_demo) printf 'smbus: whoami=6C sendrecv=6C PASS' ;;
    battery_monitor_demo) printf 'battery: soc=72%% chg=N PASS' ;;
    crc_demo) printf 'match=Y' ;;
    adc_b_demo) printf 'adc: raw=' ;;
    agt_periodic) printf 'agt: tick' ;;
    i2c_loopback) printf 'i2c: scan 0x43 ack=1' ;;
    eth_loopback) printf 'etha: loopback ok' ;;
    crypto_aes_demo) printf 'aes: round-trip OK' ;;
    dma_memcopy_demo) printf 'dma: copied 1024B match=Y' ;;
    rtc_alarm) printf 'rtc: alarm fired' ;;
    elc_event_demo) printf 'elc: en=1 trig=' ;;
    timer_capture_demo) printf 'gpt: period=' ;;
    # drw_fill_demo: the DRW is modelled INERT (board_periph_drw.c), faithful to
    # silicon where the D/AVE 2D engine never rasterizes (#247). The fill writes
    # no pixel, so the centre stays clear and the app honestly reports match=N --
    # the same result the bench gives until #247 brings the engine to life.
    drw_fill_demo) printf 'drw: fill match=N' ;;
    # drw_blend_demo: 76EFDDC5 is FNV-1a-32 over the 4096-byte ZERO framebuffer --
    # the silicon truth. The DRW never composites on hardware (#247), and the
    # board_sim model is inert to match, so the app hashes an untouched FB and
    # prints this exact banner. This is the SILICON golden (also pinned in the
    # app's hil.conf), NOT a simulator-only self-test value: SIM == HIL here.
    drw_blend_demo) printf 'drw: blit+blend crc=76EFDDC5 PASS' ;;
    dtc_transfer_demo) printf 'dtc: copied 1024B match=Y' ;;
    cac_accuracy_demo) printf 'cac: meas=ok ferr=0 ovf=0 ok=Y' ;;
    lvd_monitor_demo) printf 'lvd: pvd1 thr=2.80V mon=above det=0 ok=Y' ;;
    pdg_delay_demo) printf 'pdg: dll=on ch0=on delay=0x40 cfg=ok' ;;
    dotf_selftest_demo) printf 'dotf: ch0/1 init=ok selftest=run ok=Y' ;;
    ecc_monitor_demo) printf 'ecc: sram2 ecc=on rw=ok ok=Y' ;;
    mem_ecc_fault_demo) printf 'ecc: sram2 1bit-inj=caught 2bit-inj=caught ok=Y' ;;
    bkup_survival_demo) printf 'bkup: rw=ok survived=Y' ;;
    wdt_reset_recovery_demo) printf 'wdt: reset_by=watchdog' ;;
    lpm_idle_demo) printf 'lpm: wake_count=' ;;
    lpm_deep_sleep_demo) printf 'lpm_deep: woke' ;;
    lpm_periodic_idle) printf 'lpm_periodic_idle PASS' ;;
    import_reader) printf 'import_reader: miss->compile->cache->hit->read PASS' ;;
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
  apps=(blink lcd_color_cycle display_pal_animation ereader_ui
    ereader_chrome ereader_image ereader_link ereader_align ereader_table
    reflow_content ereader_input bscan_selftest keyboard touch_demo
    uart_hello gpt_irq_demo ssie_audio_loop crc_demo doc_demo
    canfd_loopback imu_lsm6dso_demo smbus_demo battery_monitor_demo gpio_input_demo
    adc_b_demo agt_periodic dma_memcopy_demo rtc_alarm elc_event_demo
    timer_capture_demo drw_fill_demo drw_blend_demo dtc_transfer_demo
    cac_accuracy_demo lvd_monitor_demo pdg_delay_demo
    dotf_selftest_demo ecc_monitor_demo mem_ecc_fault_demo
    bkup_survival_demo reset_cause_demo wdt_reset_recovery_demo
    lpm_idle_demo lpm_deep_sleep_demo lpm_periodic_idle
    ereader_cover ereader_svg ereader_imgfmt ereader_jpeg ereader_longstrip
    epub_parse epub_stress widget_app widget_app_demo glcdc_render
    acmphs_compare can_classic_loopback canfd_filter_demo dac_b_demo dac_waveform
    gpt_capture_input gpt_dma_demo gpt_one_shot_demo gpt_pwm_demo gpt_three_phase_demo
    i2c_loopback flash_journal eth_loopback clock_check crypto_aes_demo
    compile_on_m33 dualcore_background_m33 dualcore_mailbox
    import_reader i3c_i2c_peripheral_demo epaper_refresh modem_at_demo)
fi

echo "board_sim smoke: building the emulator ..."
cmake -B "$sim_dir/build" -S "$sim_dir" >/dev/null
cmake --build "$sim_dir/build" -j >/dev/null
sim="$sim_dir/build/board_sim"

# Build the microSD card image once if any selected app needs it.
for app in "${apps[@]}"; do
  case " $sd_apps " in
    *" $app "*)
      build_sd_image
      break
      ;;
  esac
done

# Build the EPUB-carrying card once if an import app is selected.
for app in "${apps[@]}"; do
  case " $import_reader_apps " in
    *" $app "*)
      build_book_sd_image
      break
      ;;
  esac
done

# Build the full-capacity MSC card once if the live-SD USB app is selected.
for app in "${apps[@]}"; do
  case " $usb_msc_sd_apps " in
    *" $app "*)
      build_msc_sd_image
      break
      ;;
  esac
done

fail=0
for app in "${apps[@]}"; do
  printf '  %-24s ' "$app"
  # The dual-core ITM-verdict apps must be built Debug so their INFO-level
  # ra8_log/[itm] verdict lines are compiled in (RelWithDebInfo strips them).
  # Force a clean reconfigure first: the per-app `build` target re-runs cmake
  # only when a source is newer than the ELF, so an up-to-date RelWithDebInfo
  # tree from an earlier `make <app>` would otherwise silently win and the
  # verdict would never print. BUILD_TYPE is a command-line variable, so make
  # forwards it to the per-app sub-make.
  app_make=(make "$app")
  case " $dualcore_itm_apps " in
    *" $app "*)
      app_dir="$(find examples -type d -name "$app" 2>/dev/null | head -1)"
      [ -n "$app_dir" ] && rm -rf "$app_dir/build"
      app_make=(make "$app" BUILD_TYPE=Debug)
      ;;
  esac
  if ! "${app_make[@]}" >"/tmp/smoke_build_$app.log" 2>&1; then
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
  # sim_extra_args prints zero or more whitespace-separated flags; read them
  # into an array so each flag becomes exactly one argv entry instead of
  # relying on word-splitting. The ${extra[@]+...} guard at each use site
  # keeps an empty array from tripping `set -u` under bash 3.2 (macOS).
  extra=()
  read -r -a extra <<<"$(sim_extra_args "$app")"
  # Dual-core ITM-verdict apps (see $dualcore_itm_apps): boot the single M85 .elf
  # (its Cortex-M33 image rides inside as a .cpu1_image PT_LOAD, so board_sim
  # spins up the second engine automatically). BOARD_SIM_STOP_ON ends the run at
  # the app's persistent steady-state ITM line; then assert the strong verdict --
  # which only the M33 executing can produce -- in the captured output.
  case " $dualcore_itm_apps " in
    *" $app "*)
      dc_stop="$(dualcore_stop "$app")"
      dc_want="$(dualcore_assert "$app")"
      dout="$(BOARD_SIM_STOP_ON="$dc_stop" BOARD_SIM_WALL_S=120 \
        "$sim" "$elf" 2>&1 || true)"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$dout"; then
        echo "FAULT (during dual-core run)"
        fail=1
      elif grep -qF "$dc_want" <<<"$dout"; then
        echo "OK (dual-core ITM verdict: '$dc_want')"
      else
        echo "DUAL-CORE FAIL (no ITM verdict '$dc_want' -- M33 did not run?)"
        fail=1
      fi
      continue
      ;;
  esac
  # EPUB-import apps (see $import_reader_apps): attach the baked EPUB card and
  # assert the app's full PASS banner. BOARD_SIM_STOP_ON ends the run the moment
  # the banner prints (the app idles in WFI afterwards). A generous chunk budget
  # covers the one-time compile; a missing card image (mkfontimg/Python absent)
  # surfaces here as a banner miss, which is the correct loud failure.
  case " $import_reader_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      if [ -z "$book_sd_image" ]; then
        echo "NO CARD (build_book_sd_image failed -- mkfontimg/python3 missing?)"
        fail=1
        continue
      fi
      iout="$(BOARD_SIM_STOP_ON="$want" BOARD_SIM_MAX_CHUNKS=4000000 BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" --sd "$book_sd_image" 2>&1 || true)"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$iout"; then
        echo "FAULT (during EPUB import)"
        fail=1
      elif grep -qF "$want" <<<"$iout"; then
        echo "OK (EPUB import: '$want')"
      else
        echo "IMPORT FAIL (did not reach the PASS banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # I2C/I3C target-mode apps (see $i3c_target_apps): the firmware is the
  # addressed peripheral and board_sim is the external controller. Bound the run
  # (the demo idles forever) and assert board_sim's target summary -- a non-zero
  # accepted-transaction count whose every firmware echo matched the byte the
  # synthetic controller wrote (echo=Y). That proves the responder open + status
  # + receive + send path end to end.
  case " $i3c_target_apps " in
    *" $app "*)
      tout="$(BOARD_SIM_MAX_CHUNKS=20000 BOARD_SIM_WALL_S=60 "$sim" "$elf" 2>&1 || true)"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$tout"; then
        echo "FAULT (during I2C target run)"
        fail=1
      elif grep -qF "peripheral write+read xfer(s) accepted, echo=Y" <<<"$tout"; then
        xfers="$(echo "$tout" | sed -n 's/.*target: addr=0x[0-9A-Fa-f]* \([0-9]*\) peripheral.*/\1/p' | head -1)"
        echo "OK (I2C target: $xfers write+read xfer(s), echo matched)"
      else
        echo "I2C TARGET FAIL (no accepted transaction / echo mismatch)"
        fail=1
      fi
      continue
      ;;
  esac
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
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$uout"; then
        echo "FAULT (during USB bring-up)"
        fail=1
      elif grep -q "device CONFIGURED" <<<"$uout"; then
        klass="$(echo "$uout" | sed -n 's/.*device CONFIGURED (\(.*\)).*/\1/p' | head -1)"
        echo "OK (USB enumerated -> CONFIGURED, $klass)"
      else
        echo "USB ENUM FAIL (device did not reach CONFIGURED)"
        fail=1
      fi
      continue
      ;;
  esac
  # Live-SD USB MSC device app (see $usb_msc_sd_apps): attach the 64 MiB
  # mkbookimg card, let the virtual host enumerate + run its BOT script, and
  # assert the strongest end-to-end evidence the model produces: CONFIGURED
  # (MSC active), READ CAPACITY over the USB pipe equal to the image's exact
  # block count, INQUIRY served, and a full 512-byte sector-0 read. The card
  # bring-up (400 kHz ACMD41 poll over modelled SCI-SPI) plus ThreadX/USBX
  # boot needs a bigger chunk budget than the RAM-disk enum apps;
  # BOARD_SIM_USB_STOP still ends the run shortly after enumeration. The SD
  # apps leak SPI-bus noise (incl. NULs) into the captured text, so strip
  # non-printables before bash handles it, and match with here-strings (not
  # `echo | grep -q`) to dodge the pipefail/SIGPIPE false negative.
  case " $usb_msc_sd_apps " in
    *" $app "*)
      if [ -z "$msc_sd_image" ]; then
        echo "NO CARD (build_msc_sd_image failed -- mkbookimg unavailable?)"
        fail=1
        continue
      fi
      img_blocks=$(($(wc -c <"$msc_sd_image") / 512))
      mout="$({ BOARD_SIM_MAX_CHUNKS=60000 BOARD_SIM_USB_STOP=300 BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" --sd "$msc_sd_image" 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
      msc_want="USB MSC       : capacity ${img_blocks} blocks x 512B, INQUIRY ok, sector read 512 byte(s)"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$mout"; then
        echo "FAULT (during live-SD MSC run)"
        fail=1
      elif ! grep -q "device CONFIGURED (MSC active)" <<<"$mout"; then
        echo "USB ENUM FAIL (device did not reach CONFIGURED with MSC active)"
        fail=1
      elif grep -qF "$msc_want" <<<"$mout"; then
        echo "OK (MSC enum + READ CAPACITY=${img_blocks} blocks + 512-B sector-0 read over USB)"
      else
        echo "MSC LIVE-SD FAIL (capacity/sector-read mismatch vs the ${img_blocks}-block card)"
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
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$hout"; then
        echo "FAULT (during USB host bring-up)"
        fail=1
      elif grep -qE "USB HOST (KEYBOARD|MSC BROWSE) PASS|ALL FILE OPS PASSED" <<<"$hout"; then
        banner="$(grep -oE "USB HOST [A-Z ]*PASS|ALL FILE OPS PASSED" <<<"$hout" | head -1)"
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
  # microSD ra8_io apps: blank FAT16 card via --sd-new, assert the app banner.
  case " $sd_io_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      ioclean="$({ BOARD_SIM_MAX_CHUNKS=200000 BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" --sd-new 64:fat16 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$ioclean"; then
        echo "FAULT (during ra8_io SD round-trip)"
        fail=1
      elif grep -qF "$want" <<<"$ioclean"; then
        echo "OK (ra8_io SD backend: $want)"
      else
        echo "RA8_IO SD FAIL (did not reach the PASS banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # OSPI-NOR ra8_io app: no card needed (OSPI is modelled internally), but the
  # erase-reprogram RMW is slow -- STOP_ON ends the run as soon as PASS prints.
  case " $xspi_io_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      xsclean="$({ BOARD_SIM_MAX_CHUNKS=8000000 BOARD_SIM_STOP_ON="$want" BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$xsclean"; then
        echo "FAULT (during ra8_io on-chip-NV round-trip)"
        fail=1
      elif grep -qF "$want" <<<"$xsclean"; then
        echo "OK (ra8_io on-chip-NV backend: $want)"
      else
        echo "RA8_IO on-chip-NV FAIL (did not reach the PASS banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # IT8951 e-paper app (see $eink_apps): attach the modelled controller with
  # --eink, let epaper_refresh drive the full ra8_epaper -> display-PAL e-ink
  # path (init, full refresh, partial refresh), and assert its PASS banner.
  # STOP_ON ends the run at the banner (the app heartbeats forever after); the
  # per-pixel SPI streaming needs a bigger chunk budget than the default. The SD
  # apps show SPI-bus noise can carry NULs, so strip non-printables before bash.
  case " $eink_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      epclean="$({ BOARD_SIM_MAX_CHUNKS=8000000 BOARD_SIM_STOP_ON="$want" BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" --eink 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$epclean"; then
        echo "FAULT (during e-paper refresh)"
        fail=1
      elif grep -qF "$want" <<<"$epclean"; then
        echo "OK (IT8951 e-paper: $want)"
      else
        echo "EINK FAIL (did not reach the PASS banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # Cellular AT-modem app (see $modem_apps): attach the modelled AT modem on
  # SCI7 with --modem, let modem_at_demo drive the full ra8_modem_at -> ra8_sci
  # state machine (sync, SIM, signal, registration + URC, PS attach, +CME ERROR
  # path), and assert its PASS banner. STOP_ON ends the run at the banner (the
  # app heartbeats forever after). Without --modem the modem never answers and
  # the app reports "modem: sync FAIL", so the model is load-bearing (SIM==HIL).
  case " $modem_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      mdclean="$({ BOARD_SIM_MAX_CHUNKS=4000000 BOARD_SIM_STOP_ON="$want" BOARD_SIM_WALL_S=300 \
        "$sim" "$elf" --modem 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$mdclean"; then
        echo "FAULT (during AT modem run)"
        fail=1
      elif grep -qF "$want" <<<"$mdclean"; then
        echo "OK (AT modem: $want)"
      else
        echo "MODEM FAIL (did not reach the PASS banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # Deep-idle self-parking app (see $selfpark_banner_apps): stop at the PASS
  # banner so the deliberate lpi_panic_halt low-power park is not misread as a
  # fault give-up by pc_in_halt_loop(), then assert the banner. Normal 300 s wall
  # guard, single run -- this is NOT the periodic-tick WALL_S=0 path.
  case " $selfpark_banner_apps " in
    *" $app "*)
      want="$(uart_expect "$app")"
      spout="$(BOARD_SIM_STOP_ON="$want" BOARD_SIM_WALL_S=300 "$sim" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
      if grep -q "INVALID INSN\|UNMAPPED\|executed a BKPT" <<<"$spout"; then
        echo "FAULT (during deep-idle run)"
        fail=1
      elif grep -qF "$want" <<<"$spout"; then
        echo "OK (deep-idle self-park, uart: '$want')"
      else
        echo "DEEP-IDLE FAIL (did not reach the '$want' banner)"
        fail=1
      fi
      continue
      ;;
  esac
  # Run the app on board_sim. The periodic-tick apps (see $periodic_tick_apps)
  # run with the CPU-time wall-clock guard DISABLED (BOARD_SIM_WALL_S=0) so the
  # run is bounded only by the deterministic instruction-counted chunk budget,
  # never by host CPU-time -- which is what made the banner flake under CI load
  # (#168). BOARD_SIM_STOP_ON ends the run the instant the banner prints (chunk
  # ~20) so disabling the guard does not let a genuinely-stuck app run long. The
  # banner is therefore emitted on every run regardless of host load; a small
  # retry stays as cheap insurance only. Every other app runs once with the
  # normal 300 s guard.
  tick_tries=1
  tick_stop=""
  case " $periodic_tick_apps " in
    *" $app "*)
      tick_tries=3
      tick_stop="$(uart_expect "$app")"
      ;;
  esac
  tick_want="$(uart_expect "$app")"
  for _t in $(seq 1 "$tick_tries"); do
    if [ -n "$tick_stop" ]; then
      out="$(BOARD_SIM_STOP_ON="$tick_stop" BOARD_SIM_WALL_S=0 "$sim" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
    else
      out="$(BOARD_SIM_WALL_S=300 "$sim" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
    fi
    grep -qF "$tick_want" <<<"$out" && break
  done
  if grep -q "INVALID INSN\|UNMAPPED" <<<"$out"; then
    echo "FAULT (invalid opcode / unmapped access)"
    fail=1
    continue
  fi
  # A firmware BKPT (Default_Handler's bkpt, a failed assert, a fault give-up)
  # is a halt regardless of which symbol it sits in -- board_sim stops on it
  # and prints this line. Catch it directly so a bare assert outside the named
  # *panic_halt / *_halt_loop symbols pc_in_halt_loop() knows about still fails.
  if grep -q "executed a BKPT" <<<"$out"; then
    echo "BKPT HALT ($(echo "$out" | sed -n 's/.*executed a BKPT @ *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1))"
    fail=1
    continue
  fi
  if ! grep -q "EXECUTED to the run budget" <<<"$out"; then
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
      if grep -qE "LED1[^]]*ON" <<<"$out"; then
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
      "$sim" "$elf" --ppm "$ppm" ${extra[@]+"${extra[@]}"} >/dev/null 2>&1 || true
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
    # Match with a here-string, not `echo "$out" | grep -qF`: `grep -q` closes
    # the pipe on its first match, so under this script's `pipefail` the
    # upstream `echo` dies with SIGPIPE and fails the whole compound command
    # even though the banner WAS found -- reporting UART MISMATCH for an app
    # that passed. The race needs $out to be large enough that echo is still
    # writing when grep exits, so it fires only under load: dtc_transfer_demo
    # took dev red this way while the identical binary passed on the runs
    # either side of it, logging `echo: write error: Broken pipe` one
    # millisecond before its own "mismatch". A here-string has no pipe to
    # break. Same fix as the usb_msc_sd and FS-format assertions above; this
    # generic peripheral path was the one site that still used the pipe, and
    # it covers every uart_expect app.
    if grep -qF "$want" <<<"$out"; then
      echo "OK (pc=$pc, uart: '$want')"
    else
      # Hard assertion for every app, including the periodic-tick demos: their
      # banner is now bounded by BOARD_SIM_STOP_ON and board_sim reports a
      # CPU-time truncation honestly, so a missing banner is a real failure, not
      # the old load-correlated flake (#168). No WARN-accept fallback.
      echo "UART MISMATCH (pc=$pc; expected '$want' in the SCI output)"
      fail=1
    fi
    continue
  fi
  echo "OK (pc=$pc)"
done

[ -n "$sd_image" ] && rm -f "$sd_image"
[ -n "$book_sd_image" ] && rm -f "$book_sd_image"

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
    if grep -qE "LED1[^]]*ON" <<<"$bout" && grep -qE "touch clicks  : 0 " <<<"$bout"; then
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
      if grep -qE "\[uart\] SCI8: KBDSMOKE" <<<"$kout"; then
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

# Low-battery nag (ra8_batt policy): seed the modelled fuel gauge below each
# threshold via --battery and assert the edge-triggered warning line. 15% must
# raise LOW (<=20), 8% must raise CRITICAL (<=10); the default 72% run gated
# above already proves no nag fires when healthy. Only when the app is built.
case " ${apps[*]} " in
  *" battery_monitor_demo "*)
    belf="$(find examples -path '*/battery_monitor_demo/build/battery_monitor_demo.elf' 2>/dev/null | head -1)"
    printf '  %-24s ' "battery nag LOW"
    lout="$(BOARD_SIM_STOP_ON='NAG' BOARD_SIM_WALL_S=20 BOARD_SIM_MAX_CHUNKS=6000 \
      "$sim" "$belf" --battery 15 2>&1 || true)"
    if grep -q "NAG LOW soc=15%" <<<"$lout"; then
      echo "OK (--battery 15 -> NAG LOW)"
    else
      echo "NAG FAIL (15% did not raise LOW)"
      fail=1
    fi
    printf '  %-24s ' "battery nag CRITICAL"
    cout="$(BOARD_SIM_STOP_ON='NAG' BOARD_SIM_WALL_S=20 BOARD_SIM_MAX_CHUNKS=6000 \
      "$sim" "$belf" --battery 8 2>&1 || true)"
    if grep -q "NAG CRITICAL soc=8%" <<<"$cout"; then
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
