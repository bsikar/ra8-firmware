#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/emu/smoke_apps.sh -- the app catalogue: what each app IS, and what it needs.
#
# One class per behaviour the emulator has to arrange -- an SD card image, an
# injected button or tap, a USB host, a golden frame -- with the prose that
# explains WHY each app is in that class kept beside the list it populates.
#
# SOURCED, NEVER EXECUTED. scripts/emu/smoke.sh is the only entry point; it
# sets ROOT / emu_dir / emu before sourcing, and owns the app list, the build
# phase and the summary.
#
# shellcheck disable=SC2034  # read by smoke_run.sh, sourced alongside this file
# shellcheck disable=SC2154  # ROOT and button_sw1_script are owned by smoke.sh

uart_hil_conf_index="$(
  find "$ROOT/examples" -type d -name build -prune -o -type f -name hil.conf -print | sort
)"

# UI apps whose rendered frame must be rich (distinct-color floor).
render_assert_apps="ereader_ui sd_font_render"
min_render_colors=6

# Apps driven with a held SW1 press via `--touch @PATH` ("sw1 down").
button_apps="gpio_input_demo"

# Apps driven with an injected GT911 panel tap via `--touch X,Y`.
touch_click_apps="touch_demo"

# Apps whose rendered chrome is pinned to tests/golden/ereader_chrome.
# Regenerate after an intentional change via scripts/gen/ereader_golden.py update.
golden_apps="ereader_ui"
golden_dir="$ROOT/tests/golden/ereader_chrome"

# Apps that need a FAT16 card image carrying FONT.OTF.
sd_apps="sd_font_render tz_secure_only_sd"
sd_image=""

# Apps that need the Click module fitted (LSM6DSO 0x6B + MAX17048 0x36).
click_apps="battery_monitor_demo imu_lsm6dso_demo"

# Free-running timer poll demos (AGT/RTC/ELC) where --ms + --until lets
# idle-skip advance past the wakeup period deterministically.
periodic_tick_apps="agt_periodic rtc_alarm elc_event_demo"

# USB device-enumeration apps: chapter-9 enumeration on the USBFS jack.
usb_enum_apps="usb_cdc_echo threadx_usbx_cdc_demo usb_hid_device usb_msc_device usb_printer_vendor"

# USB host-mode apps: enumerate and exercise the virtual HID / MSC peer.
usb_host_apps="usb_host_keyboard usb_host_msc_browse usb_host_file_ops"

# Live-SD USB MSC device app.
usb_msc_sd_apps="usb_msc_sdcard"

# microSD format apps: attach a blank 64 MiB FAT32 card via --sd-size / --sd-new.
sd_format_apps="fs_format_mount"

# microSD ra8_io apps: blank 64 MiB FAT16 card via --sd-size / --sd-new.
sd_io_apps="ra8_io_sd_demo ra8_io_sdhi_demo ra8_sdhi_card_demo"

# On-chip OSPI-NOR ra8_io app (OSPI is modelled internally).
xspi_io_apps="ra8_io_xspi_demo"

# IT8951 e-paper app (e-ink panel is mounted on the default EK-RA8D2 board).
eink_apps="epaper_refresh"

# Cellular AT-modem app (AT modem on SCI7 is mounted on the default board).
modem_apps="modem_at_demo"

# Deep-idle self-parking apps (stop on PASS banner before lpi_panic_halt).
selfpark_banner_apps="lpm_periodic_idle"

# Dual-core apps verified through M85 ITM log lines (`itm: ...`).
dualcore_itm_apps="dualcore_background_m33 dualcore_mailbox"

# Console substring passed to --until so ra8_emulator stops as soon as the
# dual-core handshake finishes and has logged its ITM verdict.
dualcore_stop() { # app -> persistent console stop substring on stdout
  case "$1" in
    dualcore_background_m33) printf 'dualcore_background_m33: count PASS' ;;
    dualcore_mailbox) printf 'dualcore_mailbox: 6 rounds PASS' ;;
  esac
}

# Strong `itm:` verdict asserted in captured output (only producible by CPU1).
dualcore_assert() { # app -> ITM verdict substring to assert on stdout
  case "$1" in
    dualcore_background_m33) printf 'dualcore_background_m33 PASS' ;;
    dualcore_mailbox) printf 'demo done; total M33 replies=6' ;;
  esac
}

build_sd_image() {
  local font="$ROOT/apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf"
  local mk="$ROOT/tools/mkfontimg"
  [ -f "$font" ] || return 0
  (cd "$mk" && zig build --prefix build) >/dev/null 2>&1 || return 0
  sd_image="$(mktemp -t ra8_emulator_sd.XXXXXX.img)"
  "$mk/build/bin/mkfontimg" "$font" "$sd_image" FONT.OTF >/dev/null 2>&1 || sd_image=""
}

import_reader_apps="import_reader"
book_sd_image=""

build_book_sd_image() {
  book_sd_image="$(mktemp -t ra8_emulator_book.XXXXXX.img)"
  emu_build_import_reader_card "$book_sd_image" || book_sd_image=""
}

msc_sd_image=""

build_msc_sd_image() {
  local mk="$ROOT/tools/mkbookimg"
  (cd "$mk" && zig build --prefix build) >/dev/null 2>&1 || return 0
  msc_sd_image="$(mktemp -t ra8_emulator_msc_sd.XXXXXX.img)"
  "$mk/build/bin/mkbookimg" "$msc_sd_image" "$ROOT/LICENSE.txt" >/dev/null 2>&1 || msc_sd_image=""
}

# I2C/I3C target-mode apps.
i3c_target_apps="i3c_i2c_peripheral_demo"

# Echo the extra ra8_emulator args an app needs (e.g. --sd, --touch, --click).
emu_extra_args() { # app -> extra args on stdout
  case " $sd_apps " in
    *" $1 "*) [ -n "$sd_image" ] && printf -- '--sd %s ' "$sd_image" ;;
  esac
  case " $button_apps " in
    *" $1 "*)
      [ -n "${button_sw1_script:-}" ] && printf -- '--touch @%s ' "$button_sw1_script"
      ;;
  esac
  case " $touch_click_apps " in
    *" $1 "*) printf -- '--touch 250,250 ' ;;
  esac
  case " $click_apps " in
    *" $1 "*) printf -- '--click ' ;;
  esac
}

# Expected UART banner per app -- ONE source of truth (hil.conf HIL_EXPECT).
uart_banner_apps="
  uart_hello
  ra8_io_demo ra8_io_sdram_demo compress_demo ra8_io_sd_demo
  ra8_io_sdhi_demo ra8_sdhi_card_demo ra8_io_xspi_demo ra8_io_fsfmt_demo
  ra8_io_cache_demo
  epaper_refresh modem_at_demo battery_monitor_demo bkup_survival_demo
  ereader_chrome ereader_image ereader_link ereader_align ereader_table
  reflow_content ereader_input ereader_cover ereader_svg ereader_imgfmt
  ereader_jpeg ereader_longstrip ereader_zoom epub_parse epub_stress
  widget_app widget_app_demo glcdc_render bscan_selftest keyboard
  smbus_demo crc_demo adc_b_demo agt_periodic i2c_loopback eth_loopback
  eth_gptp_timestamp_demo
  crypto_aes_demo dma_memcopy_demo rtc_alarm elc_event_demo timer_capture_demo
  drw_fill_demo drw_blend_demo dtc_transfer_demo cac_accuracy_demo
  lvd_monitor_demo pdg_delay_demo dotf_selftest_demo ecc_monitor_demo
  mem_ecc_fault_demo wdt_reset_recovery_demo lpm_idle_demo lpm_periodic_idle
  import_reader
"

# Genuine EIL != HIL divergences: keep only apps whose emulator banner differs
# intentionally from hil.conf's HIL_EXPECT.
uart_expect_override() { # app -> emulator-only expected substring, or empty
  case "$1" in
    # ra8_emulator injects --touch 250,250 so GT911 returns that coordinate.
    touch_demo) printf 'touch: open=OK pts=1 x=250 y=250' ;;
    # ra8_emulator wakes CPU0 out of deep sleep so the app reaches "woke".
    lpm_deep_sleep_demo) printf 'lpm_deep: woke' ;;
  esac
}

uart_expect_from_hil_conf() { # app -> HIL_EXPECT from its hil.conf, or empty
  local app="$1" manifest
  manifest="$(grep "/$app/hil.conf$" <<<"$uart_hil_conf_index" | head -1)"
  [ -n "$manifest" ] || return 0
  (
    HIL_EXPECT=""
    # shellcheck disable=SC1090  # runtime manifest, not a lint input
    if ! . "$manifest" >/dev/null 2>&1; then
      echo "invalid HIL manifest: $manifest" >&2
      return 1
    fi
    printf '%s' "$HIL_EXPECT"
  )
}

uart_is_banner_app() { # app -> 0 if a banner is expected, 1 otherwise
  local a
  for a in $uart_banner_apps; do
    [ "$a" = "$1" ] && return 0
  done
  return 1
}

uart_override_is_stale() { # <emu_override> <hil_expect> -> 0 iff equal (stale)
  [ "$1" = "$2" ]
}

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
