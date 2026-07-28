#!/usr/bin/env bash
#
# scripts/emu/smoke.sh -- boot each display example on the board emulator
# and assert it runs to its main loop without faulting.
#
# For every app it builds the firmware .elf and runs tools/ra8_emulator headlessly,
# then checks the run: no invalid opcode, no unmapped access, the firmware
# reached the run budget, and the final PC is NOT parked in the lcd_panic_halt
# loop. A fast regression gate for the emulator + the display bring-up path
# (clocks / SDRAM / GLCDC). For the UI apps in $render_assert_apps it ALSO
# renders one frame to a PPM and asserts the panel drew rich content (a floor on
# distinct colors) -- this folds in the pixel-content check the retired
# ui_render_check.sh used to do against the (now removed) native UI simulator.
#
#   scripts/emu/smoke.sh                 # default display apps
#   scripts/emu/smoke.sh blink lcd_draw_x  # explicit app list
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -euo pipefail

# ra8_emulator's run log and `arm-none-eabi-nm` output can carry non-UTF-8 bytes
# (register dumps, the SD font image echoed back). Under a UTF-8 locale BSD sed
# (macOS) aborts those pipelines with "RE error: illegal byte sequence"; force
# the C locale so every text tool here treats input as raw bytes. GNU tools on
# the Linux runner are unaffected either way.
export LC_ALL=C

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
emu_dir="$ROOT/tools/ra8_emulator"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328); the
# emulator build below derives from it instead of an unbounded -j.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$ROOT/scripts/ci/lib/parallelism.sh"

# ra8_emulator is C23 (typed enums, nullptr) and links C++ TUs, so its build must
# pin a C23-capable C/C++ pair -- the ambient "cc" on the Debian 12 dev box is
# gcc 12 and rejects the syntax outright (#467). Reuse the ONE shared selector
# the host-test and coverage builds use rather than hand-rolling a second probe.
# shellcheck source=scripts/builders/select_host_compiler.sh
. "$ROOT/scripts/builders/select_host_compiler.sh"

# The gate is assembled from three sourced fragments beside this file (#359).
# Split by responsibility, not size: what a run must satisfy, what each app is,
# and how one app is run. This file owns the app list, the build phase, the
# loop and the summary -- the things that only make sense once.
# shellcheck source=scripts/emu/smoke_assert.sh
. "$ROOT/scripts/emu/smoke_assert.sh"
# shellcheck source=scripts/emu/smoke_apps.sh
. "$ROOT/scripts/emu/smoke_apps.sh"
# shellcheck source=scripts/emu/smoke_run.sh
. "$ROOT/scripts/emu/smoke_run.sh"

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
    echo "  Run 'scripts/emu/smoke.sh --selftest' -- it asserts this class."
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

  # --- per-app extra args must actually REACH the emulator ---------------
  #
  # emu_extra_args() is what injects --sd / --button / --click / --reboot. If
  # its result stops being read into $extra, every app that depends on an
  # injected stimulus still builds, still runs, and still gets a verdict --
  # a WRONG one, because the stimulus never arrived. touch_demo reported
  # "UART MISMATCH" for exactly that reason after the split dropped the two
  # lines that populate $extra, while the comment explaining them survived.
  #
  # Drive the whole path: a sentinel flag out of emu_extra_args must appear in
  # the argv smoke_capture_run hands the emulator.
  args_out="$(
    set +e
    # shellcheck disable=SC2317,SC2329  # shadows the real make; invoked indirectly by smoke_build_app
    make() { return 0; }
    # shellcheck disable=SC2317,SC2329  # shadows the real find; invoked indirectly by smoke_build_app
    find() { echo "examples/probe/build/probe.elf"; }
    emu_extra_args() { printf -- '--ra8-selftest-sentinel 7'; }
    uart_expect() { echo ""; }
    periodic_tick_apps=""
    emu=/bin/echo
    smoke_build_app "probe_app" >/dev/null 2>&1
    smoke_capture_run "probe_app" >/dev/null 2>&1
    printf '%s' "$out"
  )"
  case "$args_out" in
    *"--ra8-selftest-sentinel 7"*) ;;
    *)
      echo "  FAIL emu_extra_args output never reached the emulator argv."
      echo "       Apps needing --sd / --button / --click still run and still get a"
      echo "       verdict, but without their stimulus -- a wrong PASS or a"
      echo "       mystery MISMATCH. Got argv: '$args_out'"
      sel_fail=1
      ;;
  esac

  # --- an emulator override that has CONVERGED with hil.conf is stale (#398) ------
  #
  # uart_expect() reads each app's banner from its hil.conf (EIL == HIL, the ONE
  # source of truth). uart_expect_override() is the only sanctioned exception --
  # an emulator banner that DIFFERS from hil.conf on purpose, with a stated reason. If
  # an override's value ever EQUALS the app's HIL_EXPECT, the divergence is gone
  # and the override is pure duplication again: exactly the drift #398 removed,
  # relocated into the table. Fail so it is DELETED (the app then reads hil.conf
  # like every other banner app) rather than left to rot. Without this tripwire
  # the override table just becomes the new drift site.
  #
  # The override apps are parsed from the case arms themselves, so there is no
  # second list to fall out of step with the table.
  ov_apps="$(awk '/^uart_expect_override\(\)/{f=1} f&&/^}/{f=0} f' \
    "$ROOT/scripts/emu/smoke_apps.sh" |
    grep -oE '^[[:space:]]+[a-z0-9_]+\)' | tr -d ' )')"
  if [ -z "$ov_apps" ]; then
    echo "  FAIL uart_expect_override has no parsable entries -- the staleness"
    echo "       check would police nothing"
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
      echo "       Delete it from uart_expect_override -- the app should read its"
      echo "       banner from hil.conf like every other banner app (#398)."
      sel_fail=1
    fi
  done
  # Prove the staleness predicate fires on an equal pair and stays quiet on a
  # differing one, with no app or hil.conf involved -- both directions.
  if ! uart_override_is_stale "same banner" "same banner"; then
    echo "  FAIL uart_override_is_stale did not flag a converged (equal) override"
    sel_fail=1
  fi
  if uart_override_is_stale "emulator-only banner" "bench banner"; then
    echo "  FAIL uart_override_is_stale flagged a genuinely divergent override"
    sel_fail=1
  fi

  # --- every banner app must resolve to a hil.conf HIL_EXPECT (#398) --------
  #
  # A name in uart_banner_apps with no hil.conf HIL_EXPECT would assert the
  # empty string -- i.e. pass on ANY output, the check that checks nothing. Make
  # that impossible: a listed app must supply its banner from hil.conf.
  for sapp in $uart_banner_apps; do
    if [ -z "$(uart_expect_from_hil_conf "$sapp")" ]; then
      echo "  FAIL banner app '$sapp' has no hil.conf HIL_EXPECT -- it would"
      echo "       assert the empty string (pass on any output). Add its"
      echo "       hil.conf, or drop it from uart_banner_apps."
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
  # Bare-metal apps that run identically on the CI runner's Unicorn (2.0.1) and
  # newer builds, exercising the modelled peripherals: GLCDC (display), GR1
  # framebuffer, GPIO LED, SCI UART, GPT+ICU IRQ, SSIE (I2S), CRC (crc_demo
  # self-checks hw==sw), DOC (doc_demo matches a software sum), and CAN-FD
  # (canfd_loopback round-trips a frame). The ThreadX/USBX apps (usb_cdc_echo,
  # threadx_usbx_cdc_demo, ...) drive the hand-rolled exception path on the
  # first context switch, which Unicorn 2.0.1 mis-delivers (UC_ERR_EXCEPTION);
  # they run on a newer Unicorn (macOS / a source build) -- pass them
  # explicitly there (e.g. `ra8_emulator_smoke.sh usb_cdc_echo`).
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

echo "ra8_emulator smoke: building the emulator ..."
# Pin a C23-capable host compiler (gcc-first to match CI's toolchain; falls
# back to clang where the host gcc is too old). Honours a pre-set CC/CXX and,
# under `set -e`, aborts loudly if no candidate is C23-capable -- never a silent
# fall-through to a too-old cc.
ra8_select_host_compiler gcc-14 gcc-13 gcc clang-19 clang cc
ra8_cmake_reset_if_compiler_changed "$emu_dir/build"
cmake -B "$emu_dir/build" -S "$emu_dir" \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" >/dev/null
cmake --build "$emu_dir/build" -j "$(ra8_max_jobs)" >/dev/null
emu="$emu_dir/build/ra8_emulator"

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
    bout="$("$emu" "$gelf" --click 1117 442 2>&1 || true)"
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
      kout="$(RA8_EMU_IDLE_STOP=6000 "$emu" "$kelf" --keys 'KBDSMOKE\r\n' 2>&1 || true)"
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
    lout="$(RA8_EMU_STOP_ON='NAG' RA8_EMU_WALL_S=20 RA8_EMU_MAX_CHUNKS=6000 \
      "$emu" "$belf" --battery 15 2>&1 || true)"
    if grep -q "NAG LOW soc=15%" <<<"$lout"; then
      echo "OK (--battery 15 -> NAG LOW)"
    else
      echo "NAG FAIL (15% did not raise LOW)"
      fail=1
    fi
    printf '  %-24s ' "battery nag CRITICAL"
    cout="$(RA8_EMU_STOP_ON='NAG' RA8_EMU_WALL_S=20 RA8_EMU_MAX_CHUNKS=6000 \
      "$emu" "$belf" --battery 8 2>&1 || true)"
    if grep -q "NAG CRITICAL soc=8%" <<<"$cout"; then
      echo "OK (--battery 8 -> NAG CRITICAL)"
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
