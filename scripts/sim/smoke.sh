#!/usr/bin/env bash
#
# scripts/sim/smoke.sh -- boot each display example on the board emulator
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
#   scripts/sim/smoke.sh                 # default display apps
#   scripts/sim/smoke.sh blink lcd_draw_x  # explicit app list
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

# The gate is assembled from three sourced fragments beside this file (#359).
# Split by responsibility, not size: what a run must satisfy, what each app is,
# and how one app is run. This file owns the app list, the build phase, the
# loop and the summary -- the things that only make sense once.
# shellcheck source=scripts/sim/smoke_assert.sh
. "$ROOT/scripts/sim/smoke_assert.sh"
# shellcheck source=scripts/sim/smoke_apps.sh
. "$ROOT/scripts/sim/smoke_apps.sh"
# shellcheck source=scripts/sim/smoke_run.sh
. "$ROOT/scripts/sim/smoke_run.sh"

# An app must never present as a BLANK. If the runner dies mid-app -- a helper
# leaking a conditional's status under `set -e`, a stray `exit`, anything --
# the app name has already been printed with no newline and the suite exits
# non-zero having said nothing about which app or why. That is strictly worse
# than a loud failure: it is unactionable, and with a zero exit it would be a
# silent false green.
#
# touch_demo shipped exactly that. This trap turns the blank into a named
# runner defect, so the failure mode can only ever be legible.
smoke_current_app=""
smoke_abort_guard() {
  local rc=$?
  if [ -n "$smoke_current_app" ]; then
    printf '\n'
    echo "NO VERDICT for '$smoke_current_app' -- the runner exited (status $rc) without"
    echo "  producing a result line. This is a RUNNER defect, not an app failure:"
    echo "  a helper leaked a conditional's status and 'set -e' aborted mid-app."
    echo "  Run 'scripts/sim/smoke.sh --selftest' -- it asserts this class."
  fi
  return 0
}

# --selftest: prove the gate is WIRED before trusting a green run.
#
# This gate's failure mode is not a wrong verdict, it is a missing one. Every
# app class is dispatched through $SMOKE_CLASS_VERDICTS, so a class whose entry
# is dropped stops being asserted entirely -- the app still builds, still runs,
# still prints OK from the generic path, and the class-specific check it
# actually exists for silently never happens. Splitting this file into
# fragments (#359) did exactly that to selfpark_banner_apps in an intermediate
# revision, which is why the check exists.
#
# Both directions: every verdict function must be reachable AND every class it
# names must be a non-empty app list.
if [ "${1:-}" = "--selftest" ]; then
  sel_fail=0
  defined="$(grep -ho '^smoke_verdict_[a-z0-9_]*' "$ROOT/scripts/sim/smoke_run.sh" | sort -u)"
  for fn in $defined; do
    hits=0
    for entry in "${SMOKE_CLASS_VERDICTS[@]}"; do
      [ "${entry##*:}" = "$fn" ] && hits=$((hits + 1))
    done
    grep -q "\b$fn\b" <<<"$(grep -v '^smoke_verdict_' "$ROOT/scripts/sim/smoke_run.sh")" &&
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
    if ! grep -q "^$fn() {" "$ROOT/scripts/sim/smoke_run.sh"; then
      echo "  FAIL $cls dispatches to $fn, which is not defined"
      sel_fail=1
    fi
  done
  # Must-fire direction: an entry naming a class that does not exist has to be
  # caught, otherwise the loop above proves nothing.
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
  # --- a runner helper must not leak a conditional's status as its own ----
  #
  # smoke_capture_run's retry loop ends in `grep -qF "$want" && break`. INLINE
  # that failure is exempt from `set -e` (it is a non-final command of an &&
  # list). Behind a FUNCTION boundary the same status becomes the function's
  # return value, and calling it as a plain statement aborts the caller
  # mid-line: the app name is already printed, the verdict never is, and the
  # suite exits 1 with no diagnosis at all.
  #
  # That is not hypothetical -- it is what the smoke.sh split shipped, and
  # touch_demo hit it in CI. Assert the helper returns 0 when the banner does
  # NOT match, which is the case that produced the blank.
  probe_rc=0
  (
    sim=/bin/echo
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
    echo "       behind a function boundary that aborts the caller mid-line, so the"
    echo "       app prints its name and NO verdict (the touch_demo blank)"
    sel_fail=1
  fi

  # --- the blank itself must be impossible --------------------------------
  #
  # Even if some future helper reintroduces the leak, an app may not present
  # as an empty line. Drive the abort guard directly: a runner that dies with
  # an app in flight has to name it.
  guard_out="$(
    smoke_current_app="probe_app"
    smoke_abort_guard 2>&1 || true
  )"
  case "$guard_out" in
    *"NO VERDICT for 'probe_app'"*) ;;
    *)
      echo "  FAIL the abort guard did not name the in-flight app; a runner death"
      echo "       would present as a blank line again. Got: '$guard_out'"
      sel_fail=1
      ;;
  esac
  # ...and must stay silent when no app is in flight, or every clean run ends
  # with a spurious failure report.
  quiet_out="$(
    smoke_current_app=""
    smoke_abort_guard 2>&1 || true
  )"
  if [ -n "$quiet_out" ]; then
    echo "  FAIL the abort guard fired with no app in flight: '$quiet_out'"
    sel_fail=1
  fi

  # --- per-app extra args must actually REACH the simulator ---------------
  #
  # sim_extra_args() is what injects --sd / --button / --click / --reboot. If
  # its result stops being read into $extra, every app that depends on an
  # injected stimulus still builds, still runs, and still gets a verdict --
  # a WRONG one, because the stimulus never arrived. touch_demo reported
  # "UART MISMATCH" for exactly that reason after the split dropped the two
  # lines that populate $extra, while the comment explaining them survived.
  #
  # Drive the whole path: a sentinel flag out of sim_extra_args must appear in
  # the argv smoke_capture_run hands the simulator.
  args_out="$(
    set +e
    # shellcheck disable=SC2317,SC2329  # shadows the real make; invoked indirectly by smoke_build_app
    make() { return 0; }
    # shellcheck disable=SC2317,SC2329  # shadows the real find; invoked indirectly by smoke_build_app
    find() { echo "examples/probe/build/probe.elf"; }
    sim_extra_args() { printf -- '--ra8-selftest-sentinel 7'; }
    uart_expect() { echo ""; }
    periodic_tick_apps=""
    sim=/bin/echo
    smoke_build_app "probe_app" >/dev/null 2>&1
    smoke_capture_run "probe_app" >/dev/null 2>&1
    printf '%s' "$out"
  )"
  case "$args_out" in
    *"--ra8-selftest-sentinel 7"*) ;;
    *)
      echo "  FAIL sim_extra_args output never reached the simulator argv."
      echo "       Apps needing --sd / --button / --click still run and still get a"
      echo "       verdict, but without their stimulus -- a wrong PASS or a"
      echo "       mystery MISMATCH. Got argv: '$args_out'"
      sel_fail=1
      ;;
  esac

  if [ "$sel_fail" -ne 0 ]; then
    echo "smoke.sh: --selftest FAILED"
    exit 1
  fi
  echo "smoke.sh: --selftest OK ($(wc -w <<<"$defined") verdict function(s) all dispatched;" \
    "${#SMOKE_CLASS_VERDICTS[@]} class(es) all non-empty and defined; empty-class check fires)"
  exit 0
fi

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

trap smoke_abort_guard EXIT

for app in "${apps[@]}"; do
  smoke_current_app="$app"
  smoke_run_app "$app"
  smoke_current_app=""
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
