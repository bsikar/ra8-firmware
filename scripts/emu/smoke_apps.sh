# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/emu/smoke_apps.sh -- the app catalogue: what each app IS, and what it needs.

# One class per behaviour the emulator has to arrange -- an SD card image, an
# injected button or tap, a USB host, a golden frame -- with the prose that
# explains WHY each app is in that class kept beside the list it populates.
# Separating the tables from their rationale is how a class quietly acquires a
# member nobody can justify.
#
# SOURCED, NEVER EXECUTED. scripts/emu/smoke.sh is the only entry point; it
# sets ROOT / emu_dir / emu before sourcing, and owns the app list, the build
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

# Apps driven with an injected user-button press (ra8_emulator --button 1 holds
# SW1/P009 low). gpio_input_demo mirrors SW1 -> LED1, so with the button held
# its LED1 must read ON -- this gates the GPIO input-injection path (#39).
button_apps="gpio_input_demo"

# Apps driven with an injected panel tap (ra8_emulator --click X Y arms one GT911
# contact, re-armed each chunk until the firmware's real ra8_touch_read drains
# it). touch_demo brings up the GT911 and decodes that tap, so the injected
# coordinate must come back in its banner -- this gates the GT911 touch path
# end to end (#122). 250,250 maps 1:1 on the default panel (no rotation).
touch_click_apps="touch_demo"

# Apps whose rendered chrome is pinned to a checked-in golden image (exact
# pixel match of the panel framebuffer, a strictly stronger check than the
# distinct-color floor above). ra8_emulator renders deterministically, so any
# unintended chrome change fails here. Regenerate after an intentional change
# with `make ereader-golden-update`. See scripts/gen/ereader_golden.py (#84).
golden_apps="ereader_ui"
golden_dir="$ROOT/tests/golden/ereader_chrome"

# Apps that need a microSD card attached (ra8_emulator --sd <image>). The harness
# auto-builds a small FAT16 card image (a font as FONT.OTF) and passes it so
# sd_font_render can mount + read it. This is how "specify a microSD exists" is
# exercised in CI: drop the app name here and it runs against a modelled card.
# tz_secure_only_sd does a write+read+compare roundtrip, so it needs the card
# (ra8_emulator's SD model now answers CMD24/CMD25 block writes into the image).
sd_apps="sd_font_render tz_secure_only_sd"
sd_image=""

# Apps whose UART banner is emitted by a free-running timer poll (the AGT/RTC/ELC
# tick demos). The emulation is instruction-deterministic, but ra8_emulator's
# wall-clock guard is CPU-time (clock()), so on a heavily-loaded runner Unicorn's
# TCG re-translation burned the guard's budget faster than wall time and
# TRUNCATED the run before the (deterministic, but a-few-chunks-in) banner
# printed -- and a truncated run used to be mislabelled "EXECUTED to the run
# budget", so the gate accepted it banner-less. That is the whole #168 flake: not
# emulator non-determinism, a CPU-time guard plus a mislabel. Fixed three ways:
# ra8_emulator now reports a truncation honestly (a TRUNCATED line that fails the
# budget check below instead of masquerading as success); RA8_EMU_WALL_S=0 now
# truly DISABLES that CPU-time guard; and these apps run with WALL_S=0 so they are
# bounded ONLY by the deterministic instruction-counted chunk budget, never by
# host CPU-time -- so the banner is emitted on every run regardless of load.
# RA8_EMU_STOP_ON ends each run at the banner so disabling the guard cannot make
# a genuinely-stuck app run long. The banner is a hard assertion again (no
# WARN-accept); a small retry stays as cheap insurance only.
periodic_tick_apps="agt_periodic rtc_alarm elc_event_demo"

# USB device-enumeration apps (#67 Phase 3 -- the headline USB-debugging goal).
# ra8_emulator's virtual USB host (board_usb.c) watches SYSCFG.DPRPU and drives the
# real chapter-9 SETUP sequence (GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION
# plus the class-specific traffic) against the firmware's actual USBX device stack
# + USBFS DCD register model. For these apps we assert the device reaches
# CONFIGURED with its class active -- i.e. enumeration completed end to end, with
# no hardware: CDC-ACM, HID (boot mouse), and MSC (BOT/SCSI + a sector read).
# They are ThreadX/USBX, so (like the LevelX/FileX apps) they need a newer Unicorn
# than the CI runner's 2.0.1 and a bounded budget; pass them explicitly, e.g.
# `scripts/emu/smoke.sh usb_cdc_echo usb_msc_device`.
#
# usb_printer_vendor (issue #265) is the odd one out: it is bare-metal (no
# ThreadX/USBX) and answers the same chapter-9 script from a hand-rolled polled
# responder that drives the native ra8_usb_pprn (Printer 7/1/2) + ra8_usb_pvnd
# (Vendor 0xFF) class layers. The virtual host detects its first interface
# (Printer 0x07) and the run reaches "device CONFIGURED (Printer active)"; the
# same CONFIGURED assertion applies.
usb_enum_apps="usb_cdc_echo threadx_usbx_cdc_demo usb_hid_device usb_msc_device usb_printer_vendor"

# USB HOST-mode apps (#67 Phase 3, the inverse path). ra8_emulator seams the
# first-party ra8_usb_host_* primitives to a virtual HID boot keyboard (the same
# function-seam technique it uses for ra8_eth_*), since the USBHS host controller
# (0x40351000) is unmodelled. The firmware's real host stack enumerates the
# virtual device, opens the interrupt-IN pipe, and decodes its reports -- we
# assert the app's end-to-end PASS banner. ThreadX, so newer-Unicorn-only; pass
# explicitly, e.g. `scripts/emu/smoke.sh usb_host_keyboard`.
usb_host_apps="usb_host_keyboard usb_host_msc_browse usb_host_file_ops"

# Live-SD USB MSC device app (#206): the MSC LUN serves the ACTUAL modelled
# card (--sd), not a snapshot -- media-read/media-write forward to
# ra8_sdmmc_spi_read_blocks / ra8_sdmmc_spi_write_blocks and the LUN geometry is
# the card's CSD capacity. ra8_emulator's virtual USB host enumerates the device
# and drives the MSC BOT script (INQUIRY, READ CAPACITY(10), READ(10) of
# sector 0), so the gate asserts CONFIGURED (MSC active) PLUS the capacity
# reported over the USB pipe equals the card image's real block count PLUS a
# full 512-byte sector-0 read -- proving capacity + data flow card ->
# ra8_sdmmc_spi -> media callbacks -> USB end to end. The scripted host issues
# no WRITE(10); the real-PC file copy stays a manual bench step (README.md).
# ThreadX/USBX, so newer-Unicorn-only; pass explicitly, e.g.
# `scripts/emu/smoke.sh usb_msc_sdcard`.
usb_msc_sd_apps="usb_msc_sdcard"

# microSD FORMAT apps: exercise the ra8_fs_format() mkfs path end to end. These
# reformat the modelled card themselves (once per FAT type), so they take a
# blank card via ra8_emulator's --sd-new (not a pre-built --sd image). ra8_emulator's
# SD model answers the block writes the formatter and ra8_fs emit, and its CSD
# reports the --sd-new size so the SD bring-up sees a real capacity.
# fs_format_mount formats + mounts + file-cycles FAT12, FAT16, FAT32 (+ an
# exFAT format + mount + empty-root trial) and
# prints "FS FORMAT+MOUNT ALL PASS". Bare-metal (no ThreadX), so it runs on the
# CI runner's Unicorn too, but it is opt-in like the other card apps: pass it
# explicitly, e.g. `scripts/emu/smoke.sh fs_format_mount`.
sd_format_apps="fs_format_mount"

# microSD ra8_io apps: prove the ra8_io fabric's swappable SD-over-SPI block-device
# backend (ra8_io_blockdev_sdspi) by formatting + mounting FAT16 on a blank
# --sd-new card and round-tripping a file through the VFS. Distinct from the
# fs_format_mount banner, so it gets its own banner assertion via uart_expect().
sd_io_apps="ra8_io_sd_demo ra8_io_sdhi_demo ra8_sdhi_card_demo"

# On-chip non-volatile ra8_io apps (no CLI flag -- ra8_emulator models the medium
# internally): OSPI NOR (ra8_io_xspi_demo, erase-before-write 4 KiB RMW). It
# idles forever after its PASS banner, so STOP_ON ends the run the moment it
# prints. Asserts via uart_expect().
#
# ra8_io_mram_demo was here and has been REMOVED (#170). It targets a
# general-purpose data-flash at 0x2700_0000 that this silicon does not have:
# HUM Ch 5 Figure 5.2 p 237 labels the region "Extra MRAM (option-setting
# memory)", HUM Ch 59.7.4.5 Table 59.15 p 3592 lists every legal MACI Program
# target (all option-setting / OTP, 0x02E0_7600..0x02E1_79F0), and 0x2700_0000
# appears nowhere in the manual. The bench returns Error=516 with the sequencer
# command-locked, and board_periph_mram.c now reproduces that rejection instead
# of writing the payload into mapped RAM. So the demo cannot pass, and asserting
# a PASS banner for it made this gate claim a storage backend the part does not
# have. Its hil.conf carries the full evidence and the delete-or-repoint
# decision; re-add here only if it is ever repointed at real storage.
xspi_io_apps="ra8_io_xspi_demo"

# IT8951 e-paper apps (#256): ra8_emulator attaches a modelled IT8951 controller on
# SPI_B with --eink (board_periph_eink.c), which answers HRDY, the GET_DEV_INFO
# drain and the LUTAFSR "LUT idle" poll, so epaper_refresh drives the full
# ra8_epaper -> display-PAL e-ink -> ra8_io_spi_bus path to its PASS banner with
# no panel. The per-pixel SPI streaming (full + partial refresh) needs a bigger
# chunk budget than the default; STOP_ON ends the run at the banner. Without
# --eink HRDY never asserts and the app honestly reports FAIL, so the model is
# load-bearing (EIL == HIL). Asserts via uart_expect().
eink_apps="epaper_refresh"

# Cellular AT-modem app (#259): ra8_emulator attaches a modelled 3GPP AT modem on
# the MikroBUS UART (SCI7) with --modem (board_periph_modem.c), which answers
# the demo's AT script -- AT / ATE0 / CMEE / CPIN? / CSQ / CREG=1 (+ a +CREG
# URC) / CREG? / CGATT? -- and rejects an unsupported command with +CME ERROR,
# so modem_at_demo drives the full ra8_modem_at -> ra8_sci path (sync, SIM,
# signal, registration, PS attach, error path) to its PASS banner with no
# physical modem. Without --modem the modem never answers and the app honestly
# reports "modem: sync FAIL", so the model is load-bearing (EIL == HIL for the
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
# stimulus port; ra8_emulator surfaces those bytes as `[itm] ...` lines (the SWO
# analog of the `[uart]` console echo). The M85 narrates the M33's replies too
# -- a dualcore_mailbox reply of operand*3+1, or the background counter's final
# total, can only come from the SECOND core actually executing -- so the ITM
# verdict gates the dual-core release + cross-core shared-SRAM IPC path end to
# end, with no hardware. They are built Debug (see the build loop) because
# ra8_log_info is compiled to a no-op at the default RelWithDebInfo level, and run
# with RA8_EMU_STOP_ON tied to each app's PERSISTENT steady-state ITM line so
# the run ends deterministically the moment the verdict has been logged (the M85
# parks / heartbeats forever afterwards). Bare-metal (no ThreadX), and the
# already-gated compile_on_m33 proves the CI runner's Unicorn spins up the second
# (M33) engine, so these run in the default list too.
dualcore_itm_apps="dualcore_background_m33 dualcore_mailbox"

# Persistent `[itm]` substring RA8_EMU_STOP_ON waits for: the M85 keeps logging
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
  sd_image="$(mktemp -t ra8_emulator_sd.XXXXXX.img)"
  "$mk/build/mkfontimg" "$font" "$sd_image" FONT.OTF >/dev/null 2>&1 || sd_image=""
}

# Apps that import a `.epub` off the SD card and compile it to a cached `.rabook`
# on first open (#151). import_reader mounts the card, finds BOOK.EPB, compiles
# it once (cache MISS), reopens it (cache HIT, no recompile), then reads the
# cached book back -- printing "import_reader: miss->compile->cache->hit->read
# PASS". A BLANK card has no BOOK.EPB, so it fails at the source open; this gate
# bakes a tiny deterministic EPUB onto the card (build_book_sd_image) so the full
# import + compile + cache + read path runs end to end in ra8_emulator with no
# hardware. The seed book carries a `text/css` stylesheet (style.css) so the run
# exercises the runtime ra8_rabook stylesheet-compile stage end to end, not just
# text (#169). This was previously gated text-only on the belief it tripped a
# ra8_emulator emulation gap; the real cause was a firmware bug -- import_reader's
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
  epub="$(mktemp -t ra8_emulator_book.XXXXXX.epub)"
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
  book_sd_image="$(mktemp -t ra8_emulator_book.XXXXXX.img)"
  "$mk/build/mkfontimg" "$epub" "$book_sd_image" BOOK.EPB >/dev/null 2>&1 || book_sd_image=""
  rm -f "$epub"
}

msc_sd_image=""

# Build the full-capacity card image for the live-SD USB MSC gate. mkbookimg
# always emits a 64 MiB FAT32 image (131072 x 512-B sectors -- an exact
# 512 KiB multiple, so ra8_emulator's CSD synthesis reports precisely that block
# count and the capacity assertion is byte-exact). The baked file just makes
# the volume non-empty; LICENSE.txt is always present in the repo. Sets the
# global $msc_sd_image on success; leaves it empty if mkbookimg is
# unavailable (the gate then fails loudly -- the card IS the test medium).
build_msc_sd_image() {
  local mk="$ROOT/tools/mkbookimg"
  cmake -B "$mk/build" -S "$mk" >/dev/null 2>&1 || return 0
  cmake --build "$mk/build" >/dev/null 2>&1 || return 0
  msc_sd_image="$(mktemp -t ra8_emulator_msc_sd.XXXXXX.img)"
  "$mk/build/mkbookimg" "$msc_sd_image" "$ROOT/LICENSE.txt" >/dev/null 2>&1 || msc_sd_image=""
}

# I2C/I3C TARGET-mode apps. i3c_i2c_peripheral_demo brings IIC_B up as an
# addressed peripheral (it programmes its own MSDVAD address and waits to be
# addressed) and emits NO UART -- the only on-chip signal is LED1, which toggles
# on every accepted transaction. ra8_emulator's I2C model now plays the EXTERNAL
# controller (board_periph_i2c.c i3c_periph_*): it writes a byte the firmware
# drains, then reads the firmware's one-byte echo, so the responder driver runs
# end to end with no wired-up bus. ra8_emulator prints a target summary line at run
# end ("I3C/I2C target: ... echo=Y"); the run is bounded by a small chunk budget
# (the demo idles forever otherwise) and the echo verdict is deterministic the
# moment any transaction completes.
i3c_target_apps="i3c_i2c_peripheral_demo"

# Echo the extra ra8_emulator args an app needs (e.g. --sd <image> for SD apps).
emu_extra_args() { # app -> extra args on stdout
  case " $sd_apps " in
    *" $1 "*) [ -n "$sd_image" ] && printf -- '--sd %s' "$sd_image" ;;
  esac
  case " $button_apps " in
    *" $1 "*) printf -- '--button 1' ;;
  esac
  case " $touch_click_apps " in
    *" $1 "*) printf -- '--click 250 250' ;;
  esac
  # bkup_survival_demo proves reset-survival: --reboot makes the emulator re-run the
  # firmware from its reset vector with the VBATT backup domain retained, so the
  # second boot finds the sentinel and reports survived=Y.
  case "$1" in
    bkup_survival_demo) printf -- '--reboot 1' ;;
  esac
}

# Expected UART banner per app -- ONE source of truth (#398).
#
# The banner an app must print lives in exactly ONE place: HIL_EXPECT in the
# app's hil.conf. The owner's EIL == HIL rule makes hil.conf authoritative, so
# uart_expect() READS it from there rather than hand-copying it. The old copy
# here had already drifted from several hil.confs, and a stale copy does not
# fail loudly -- it asserts the wrong thing and passes, or fails pointing
# nowhere near the cause. This upgrades the gate from "booted without faulting"
# to "produced the right peripheral data" (e.g. crc_demo's hw CRC must equal its
# sw CRC, adc must read midscale, the RTC alarm must fire) with no second copy
# to rot.
#
# Two small tables replace the ~60 hand-copied strings:
#   uart_banner_apps      -- WHICH apps the emulator asserts a banner for.
#                            Membership ONLY: no banner string lives here, so
#                            there is nothing to drift. An app absent from this
#                            list (blink, doc_demo, display apps, ...) is judged
#                            on "ran to the run budget without faulting" alone,
#                            exactly as before.
#   uart_expect_override  -- the few apps whose EMULATOR banner legitimately
#                            differs from the bench (each with an inline reason).
#                            The ONLY sanctioned place an emulator expectation may
#                            differ from hil.conf; smoke.sh --selftest FAILS on
#                            any entry that has CONVERGED with its hil.conf.

# Apps whose SCI banner the emulator asserts, keyed by NAME only -- the banner
# STRING is read from each app's hil.conf HIL_EXPECT (EIL == HIL). Keeping only
# names here means there is no string to fall out of step with hil.conf.
# smoke.sh --selftest fails if any name here resolves to no hil.conf HIL_EXPECT
# (a listed app that cannot supply its banner would silently assert the empty
# string -- pass on any output -- which is exactly the hole #398 closes).
uart_banner_apps="
  uart_hello
  ra8_io_demo ra8_io_sdram_demo ra8_io_compress_demo ra8_io_sd_demo
  ra8_io_sdhi_demo ra8_sdhi_card_demo ra8_io_xspi_demo ra8_io_fsfmt_demo
  ra8_io_cache_demo
  epaper_refresh modem_at_demo battery_monitor_demo
  ereader_chrome ereader_image ereader_link ereader_align ereader_table
  reflow_content ereader_input ereader_cover ereader_svg ereader_imgfmt
  ereader_jpeg ereader_longstrip epub_parse epub_stress
  widget_app widget_app_demo glcdc_render bscan_selftest keyboard
  smbus_demo crc_demo adc_b_demo agt_periodic i2c_loopback eth_loopback
  crypto_aes_demo dma_memcopy_demo rtc_alarm elc_event_demo timer_capture_demo
  drw_fill_demo drw_blend_demo dtc_transfer_demo cac_accuracy_demo
  lvd_monitor_demo pdg_delay_demo dotf_selftest_demo ecc_monitor_demo
  mem_ecc_fault_demo wdt_reset_recovery_demo lpm_idle_demo lpm_periodic_idle
  import_reader
"
# ra8_io_mram_demo is deliberately absent (#170): the extra-MRAM data region it
# targets does not exist on this silicon, so it prints no PASS banner in the
# emulator or on the bench and is run by no gate.

# Genuine EIL != HIL divergences: the EMULATOR asserts a banner the bench
# deliberately cannot. Each entry pins its emulator-only banner HERE (not in
# hil.conf) and states WHY. THE ONLY sanctioned place an emulator expectation may
# differ from hil.conf's HIL_EXPECT; smoke.sh --selftest rejects any entry that
# has CONVERGED with hil.conf (equal values -> the divergence is gone -> delete
# the entry so the app reads hil.conf like every other banner app).
uart_expect_override() { # app -> emulator-only expected substring, or empty
  case "$1" in
    # ra8_emulator injects a fixed --click 250 250 tap, so the GT911 decode returns
    # that exact coordinate and the emulator gates the touch-injection path end to
    # end (#122). A bench tap lands wherever the operator presses, so hil.conf
    # asserts only "touch: open=OK" (HIL_EXPECT_SHORT_OK: a no-touch read is
    # also OK) -- the coordinate cannot be pinned on hardware.
    touch_demo) printf 'touch: open=OK pts=1 x=250 y=250' ;;
    # smoke runs this with --reboot 1, so ra8_emulator re-enters from the reset
    # vector with the VBATT backup domain retained and the SECOND boot finds the
    # sentinel -> survived=Y. The bench (hil.conf: "bkup: rw=ok") runs a single
    # boot, never re-enters, so survival is unobservable there.
    bkup_survival_demo) printf 'bkup: rw=ok survived=Y' ;;
    # ra8_emulator wakes the core out of deep sleep, so the app reaches "woke". On
    # silicon deep sleep cannot wake on this stimulus, so the bench (hil.conf:
    # "lpm_deep: boot") only ever sees the boot line.
    lpm_deep_sleep_demo) printf 'lpm_deep: woke' ;;
  esac
}

# Read an app's authoritative banner (HIL_EXPECT) from its hil.conf. Empty if
# the app has no hil.conf, or its manifest declares no HIL_EXPECT. Sourced in a
# subshell so nothing the manifest assigns leaks into the caller.
uart_expect_from_hil_conf() { # app -> HIL_EXPECT from its hil.conf, or empty
  local app="$1" dir
  dir="$(find "$ROOT/examples" -type d -name "$app" 2>/dev/null | grep -v '/build/' | head -1)"
  [ -n "$dir" ] || return 0
  [ -f "$dir/hil.conf" ] || return 0
  (
    HIL_EXPECT=""
    # shellcheck disable=SC1091  # per-app manifest resolved at runtime, not a lint input
    . "$dir/hil.conf" >/dev/null 2>&1 || true
    printf '%s' "$HIL_EXPECT"
  )
}

# True (0) when the emulator asserts a UART banner for $1. Word-splits the list
# so a multi-line uart_banner_apps matches (a `case " $list "` glob would miss a
# name adjacent to a newline).
uart_is_banner_app() { # app -> 0 if a banner is expected, 1 otherwise
  local a
  for a in $uart_banner_apps; do
    [ "$a" = "$1" ] && return 0
  done
  return 1
}

# Staleness predicate for an override: 0 (stale) iff the emulator override value has
# CONVERGED with hil.conf's HIL_EXPECT (they are equal), meaning the divergence
# the override records no longer exists. Kept as a 2-arg pure function so
# smoke.sh --selftest can prove it fires (equal) and stays quiet (differing)
# with no app or hil.conf involved.
uart_override_is_stale() { # <emu_override> <hil_expect> -> 0 iff equal (stale)
  [ "$1" = "$2" ]
}

# Expected SCI banner substring for an app, or empty for apps judged only on
# "ran to the run budget without faulting" (display / button / render apps, and
# every app not in uart_banner_apps). ra8_emulator emits SCI TX as
# `[uart] SCIn: ...`; smoke_run.sh greps this substring in that output and also
# uses it as the RA8_EMU_STOP_ON marker.
uart_expect() { # app -> expected UART substring on stdout
  local app="$1" ov
  ov="$(uart_expect_override "$app")"
  if [ -n "$ov" ]; then
    printf '%s' "$ov"
    return 0
  fi
  if uart_is_banner_app "$app"; then
    uart_expect_from_hil_conf "$app"
  fi
}
