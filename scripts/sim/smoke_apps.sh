# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/sim/smoke_apps.sh -- the app catalogue: what each app IS, and what it needs.

# One class per behaviour the emulator has to arrange -- an SD card image, an
# injected button or tap, a USB host, a golden frame -- with the prose that
# explains WHY each app is in that class kept beside the list it populates.
# Separating the tables from their rationale is how a class quietly acquires a
# member nobody can justify.
#
# SOURCED, NEVER EXECUTED. scripts/sim/smoke.sh is the only entry point; it
# sets ROOT / sim_dir / sim before sourcing, and owns the app list, the build
# phase and the summary.
#
# shellcheck disable=SC2034  # every list here is READ by smoke_run.sh, which is sourced alongside this file; shellcheck cannot see across the two
# shellcheck disable=SC2154  # ROOT is set by smoke.sh before this file is sourced

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
# with `make ereader-golden-update`. See scripts/gen/ereader_golden.py (#84).
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
# `scripts/sim/smoke.sh usb_cdc_echo usb_msc_device`.
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
# explicitly, e.g. `scripts/sim/smoke.sh usb_host_keyboard`.
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
# `scripts/sim/smoke.sh usb_msc_sdcard`.
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
# explicitly, e.g. `scripts/sim/smoke.sh fs_format_mount`.
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
# scripts/gen/rabook_parity_gen.py uses), then writes it through mkfontimg's
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
