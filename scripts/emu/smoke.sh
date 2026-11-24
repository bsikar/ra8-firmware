#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/emu/smoke.sh -- boot each display/peripheral example on the board
# emulator and assert it runs to its budget without faulting.
#
# For every app it builds the firmware .elf and runs tools/ra8_emulator headlessly,
# checking that no fault / BKPT occurred, the run finished cleanly, and the final
# PC is not parked in a halt loop. For display apps in $render_assert_apps it
# also captures panel frames via --frames-out and checks distinct pixel colors
# (and matches tests/golden/ereader_chrome for ereader_ui).
#
#   scripts/emu/smoke.sh                 # default curated apps
#   scripts/emu/smoke.sh blink lcd_draw_x  # explicit app list
#
set -euo pipefail
export LC_ALL=C

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
emu_dir="$ROOT/tools/ra8_emulator"

# shellcheck source=scripts/lib/parallelism.sh
. "$ROOT/scripts/lib/parallelism.sh"

# shellcheck source=scripts/emu/smoke_assert.sh
. "$ROOT/scripts/emu/smoke_assert.sh"
# shellcheck source=scripts/emu/emu_fixtures.sh
. "$ROOT/scripts/emu/emu_fixtures.sh"
# shellcheck source=scripts/emu/smoke_apps.sh
. "$ROOT/scripts/emu/smoke_apps.sh"
# shellcheck source=scripts/emu/smoke_run.sh
. "$ROOT/scripts/emu/smoke_run.sh"

smoke_current_app=""
smoke_abort_guard() {
  local rc=$?
  if [ -n "$smoke_current_app" ]; then
    printf '\n'
    echo "NO VERDICT for '$smoke_current_app' -- the runner exited (status $rc) without"
    if [ "$rc" -eq 0 ]; then
      echo "  producing a result line. Status 0 usually means the runner was killed from"
      echo "  outside (an outer 'timeout' cap or a signal), not an internal abort:"
      echo "  check the cap that wrapped this run before reading it as a runner defect."
    else
      echo "  producing a result line. A nonzero status points at an internal abort:"
      echo "  a helper leaked a conditional's status and 'set -e' aborted mid-app."
    fi
    echo "  Run 'scripts/emu/smoke.sh --selftest' -- it asserts this class."
  fi
  return 0
}

# --selftest: prove the gate is WIRED before trusting a green run.
if [ "${1:-}" = "--selftest" ]; then
  sel_fail=0
  defined="$(grep -ho '^smoke_verdict_[a-z0-9_]*' "$ROOT/scripts/emu/smoke_run.sh" | sort -u)"
  for fn in $defined; do
    hits=0
    for entry in "${SMOKE_CLASS_VERDICTS[@]}"; do
      [ "${entry##*:}" = "$fn" ] && hits=$((hits + 1))
    done
    grep -q "\b$fn\b" <<<"$(grep -v '^smoke_verdict_' "$ROOT/scripts/emu/smoke_run.sh")" &&
      hits=$((hits + 1))
    if [ "$hits" -eq 0 ]; then
      echo "  FAIL $fn is defined but nothing dispatches to it -- its app class is unasserted"
      sel_fail=1
    fi
  done
  for entry in "${SMOKE_CLASS_VERDICTS[@]}"; do
    cls="${entry%%:*}"
    fn="${entry##*:}"
    if [ -z "${!cls:-}" ]; then
      echo "  FAIL class list \$$cls is empty or unset -- $fn can never run"
      sel_fail=1
    fi
    if ! grep -q "^$fn() {" "$ROOT/scripts/emu/smoke_run.sh"; then
      echo "  FAIL $cls dispatches to $fn, which is not defined"
      sel_fail=1
    fi
  done
  SMOKE_CLASS_VERDICTS+=("ra8_no_such_apps:smoke_verdict_dualcore_itm")
  if [ -n "${ra8_no_such_apps:-}" ]; then
    echo "  FAIL selftest fixture is not synthetic"
    sel_fail=1
  fi
  probe=0
  for entry in "${SMOKE_CLASS_VERDICTS[@]}"; do
    cls="${entry%%:*}"
    [ -z "${!cls:-}" ] && probe=1
  done
  if [ "$probe" -ne 1 ]; then
    echo "  FAIL the empty-class check does not fire on a synthetic empty class"
    sel_fail=1
  fi
  unset 'SMOKE_CLASS_VERDICTS[${#SMOKE_CLASS_VERDICTS[@]}-1]'

  probe_rc=0
  (
    emu=/bin/echo
    elf=""
    extra=()
    uart_expect() { echo "a-banner-no-run-will-ever-print"; }
    periodic_tick_apps=""
    set +e
    smoke_capture_run "probe_app" >/dev/null 2>&1
    exit $?
  ) || probe_rc=$?
  if [ "$probe_rc" -ne 0 ]; then
    echo "  FAIL smoke_capture_run returned $probe_rc when the banner did not match;"
    echo "       behind a function boundary that aborts the caller mid-line."
    sel_fail=1
  fi

  guard_out="$(
    smoke_current_app="probe_app"
    smoke_abort_guard 2>&1 || true
  )"
  case "$guard_out" in
    *"NO VERDICT for 'probe_app'"*) ;;
    *)
      echo "  FAIL the abort guard did not name the in-flight app. Got: '$guard_out'"
      sel_fail=1
      ;;
  esac
  quiet_out="$(
    smoke_current_app=""
    smoke_abort_guard 2>&1 || true
  )"
  if [ -n "$quiet_out" ]; then
    echo "  FAIL the abort guard fired with no app in flight: '$quiet_out'"
    sel_fail=1
  fi

  # Drive the whole extra-args path: emu_extra_args() must reach $emu argv.
  args_out="$(
    set +e
    probe_dir="$(mktemp -d)"
    fake_just="$probe_dir/just"
    printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_just"
    chmod +x "$fake_just"
    export RA8_JUST="$fake_just"
    mkdir -p "$probe_dir/build"
    : >"$probe_dir/build/probe_app.elf"
    smoke_app_dir() { echo "$probe_dir"; }
    emu_extra_args() { printf -- '--ra8-selftest-sentinel 7'; }
    uart_expect() { echo ""; }
    periodic_tick_apps=""
    emu=/bin/echo
    smoke_build_app "probe_app" >/dev/null 2>&1
    smoke_capture_run "probe_app" >/dev/null 2>&1
    rm -rf "$probe_dir"
    printf '%s' "$out"
  )"
  case "$args_out" in
    *"--ra8-selftest-sentinel 7"*) ;;
    *)
      echo "  FAIL emu_extra_args output never reached the emulator argv."
      echo "       Got argv: '$args_out'"
      sel_fail=1
      ;;
  esac

  ov_apps="$(awk '/^uart_expect_override\(\)/{f=1} f&&/^}/{f=0} f' \
    "$ROOT/scripts/emu/smoke_apps.sh" |
    grep -oE '^[[:space:]]+[a-z0-9_]+\)' | tr -d ' )')"
  if [ -z "$ov_apps" ]; then
    echo "  FAIL uart_expect_override has no parsable entries"
    sel_fail=1
  fi
  for sapp in $ov_apps; do
    sov="$(uart_expect_override "$sapp")"
    she="$(uart_expect_from_hil_conf "$sapp")"
    if [ -z "$sov" ]; then
      echo "  FAIL override '$sapp' parsed from the table returns no value"
      sel_fail=1
    elif [ -z "$she" ]; then
      echo "  FAIL override '$sapp' has no hil.conf HIL_EXPECT to diverge FROM"
      sel_fail=1
    elif uart_override_is_stale "$sov" "$she"; then
      echo "  FAIL override '$sapp' has CONVERGED with hil.conf (emulator=hil='$sov')."
      sel_fail=1
    fi
  done
  if ! uart_override_is_stale "same banner" "same banner"; then
    echo "  FAIL uart_override_is_stale did not flag a converged (equal) override"
    sel_fail=1
  fi
  if uart_override_is_stale "emulator-only banner" "bench banner"; then
    echo "  FAIL uart_override_is_stale flagged a genuinely divergent override"
    sel_fail=1
  fi

  for sapp in $uart_banner_apps; do
    if [ -z "$(uart_expect_from_hil_conf "$sapp")" ]; then
      echo "  FAIL banner app '$sapp' has no hil.conf HIL_EXPECT"
      sel_fail=1
    fi
  done

  if [ "$sel_fail" -ne 0 ]; then
    echo "smoke.sh: --selftest FAILED"
    exit 1
  fi
  echo "smoke.sh: --selftest OK ($(wc -w <<<"$defined") verdict function(s) all dispatched;" \
    "${#SMOKE_CLASS_VERDICTS[@]} class(es) all non-empty and defined; empty-class check fires;" \
    "$(wc -w <<<"$ov_apps") override(s) non-stale;" \
    "$(wc -w <<<"$uart_banner_apps") banner app(s) resolve to a hil.conf HIL_EXPECT)"
  exit 0
fi

apps=("$@")
if [ "${#apps[@]}" -eq 0 ]; then
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
    ereader_zoom
    epub_parse epub_stress widget_app widget_app_demo glcdc_render
    acmphs_compare can_classic_loopback canfd_filter_demo dac_b_demo dac_waveform
    gpt_capture_input gpt_dma_demo gpt_one_shot_demo gpt_pwm_demo gpt_three_phase_demo
    i2c_loopback flash_journal eth_loopback eth_gptp_timestamp_demo
    clock_check crypto_aes_demo
    compile_on_m33 dualcore_background_m33 dualcore_mailbox
    import_reader i3c_i2c_peripheral_demo epaper_refresh modem_at_demo)
else
  resolved_apps=()
  for app_selector in "${apps[@]}"; do
    app_name="$(python3 scripts/dev/ra8_apps.py name "$app_selector")" || {
      echo "FATAL: unknown or ambiguous app '$app_selector'" >&2
      exit 2
    }
    resolved_apps+=("$app_name")
  done
  apps=("${resolved_apps[@]}")
fi

# The gate calls bare `zig` and `arm-none-eabi-nm`, and the app builds configure
# through cmake/toolchain-ra8d2.cmake. On the runner and in the devcontainer all
# of it is on PATH; on a dev box it often lives in user-space installs instead
# (the Arm toolchain under ~/opt, a zig distribution beside an older one,
# cmake/just/ninja in ~/.local/bin). Discover and export it here -- RA8_ZIG and
# RA8_ARM_TOOLCHAIN_BIN keep their documented precedence -- so a local smoke
# run needs no hand-built PATH and no sudo.
# shellcheck source=scripts/builders/find_tools.sh
. "$ROOT/scripts/builders/find_tools.sh"
ra8_find_tools_activate

if [ -n "${RA8_EMULATOR:-}" ]; then
  emu="$RA8_EMULATOR"
  echo "ra8_emulator smoke: using prebuilt RA8_EMULATOR -> $emu"
else
  if [ ! -f "$emu_dir/build.zig" ]; then
    echo "FATAL: tools/ra8_emulator is a submodule (bsikar/ra8-emulator) and is not checked out." >&2
    echo "       Run 'git submodule update --init tools/ra8_emulator' and retry." >&2
    exit 1
  fi
  echo "ra8_emulator smoke: building the emulator (zig build) ..."
  (cd "$emu_dir" && zig build -Doptimize=ReleaseFast) >/dev/null
  emu="$emu_dir/zig-out/bin/ra8_emulator"
fi
if [ ! -x "$emu" ]; then
  echo "FATAL: ra8_emulator binary missing or not executable: $emu" >&2
  exit 1
fi

button_sw1_script=""
# shellcheck disable=SC2329  # invoked by the EXIT trap installed below.
smoke_cleanup() {
  smoke_abort_guard
  [ -n "$sd_image" ] && rm -f "$sd_image"
  [ -n "$book_sd_image" ] && rm -f "$book_sd_image"
  [ -n "$msc_sd_image" ] && rm -f "$msc_sd_image"
  [ -n "$button_sw1_script" ] && rm -f "$button_sw1_script"
}
trap smoke_cleanup EXIT

for app in "${apps[@]}"; do
  case " $button_apps " in
    *" $app "*)
      button_sw1_script="$(mktemp -t ra8_sw1.XXXXXX)"
      printf 'sw1 down\n' >"$button_sw1_script"
      break
      ;;
  esac
done

for app in "${apps[@]}"; do
  case " $sd_apps " in
    *" $app "*)
      build_sd_image
      break
      ;;
  esac
done

for app in "${apps[@]}"; do
  case " $import_reader_apps " in
    *" $app "*)
      build_book_sd_image
      break
      ;;
  esac
done

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
  smoke_current_app="$app"
  smoke_run_app "$app"
  smoke_current_app=""
done

# Battery nag threshold assertions (ra8_batt policy): 15% must raise LOW and
# 8% must raise CRITICAL; default 72% above proves healthy level does not nag.
case " ${apps[*]} " in
  *" battery_monitor_demo "*)
    belf="$(smoke_app_dir battery_monitor_demo)/build/battery_monitor_demo.elf"
    printf '  %-24s ' "battery nag LOW"
    lout="$("$emu" "$belf" --console --click --battery 15 --until NAG --ms 5000 </dev/null 2>&1 || true)"
    if grep -q "NAG LOW soc=15%" <<<"$lout"; then
      echo "OK (--click --battery 15 -> NAG LOW)"
    else
      echo "NAG FAIL (15% did not raise LOW)"
      fail=1
    fi
    printf '  %-24s ' "battery nag CRITICAL"
    cout="$("$emu" "$belf" --console --click --battery 8 --until NAG --ms 5000 </dev/null 2>&1 || true)"
    if grep -q "NAG CRITICAL soc=8%" <<<"$cout"; then
      echo "OK (--click --battery 8 -> NAG CRITICAL)"
    else
      echo "NAG FAIL (8% did not raise CRITICAL)"
      fail=1
    fi
    ;;
esac

if [ "$fail" -eq 0 ]; then
  echo "ra8_emulator smoke: ALL PASS"
  exit 0
fi
echo "ra8_emulator smoke: FAILURES"
exit 1
