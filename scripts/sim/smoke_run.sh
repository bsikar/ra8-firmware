# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/sim/smoke_run.sh -- run ONE app and assert its result.

# The per-app-class dispatch chain. Each 'case $<class>_apps' arm is that
# class's whole verdict, and returns as soon as it has one; the generic
# fault / run-budget / halt-loop checks at the end apply to every app that
# reaches them.
#
# Assigns the caller's $fail on any failure, exactly as the inline loop it
# was extracted from did.
#
# SOURCED, NEVER EXECUTED. scripts/sim/smoke.sh is the only entry point; it
# sets ROOT / sim_dir / sim before sourcing, and owns the app list, the build
# phase and the summary.
#
# shellcheck disable=SC2154  # the app-class lists come from smoke_apps.sh and ROOT/sim/elf/fail from smoke.sh, both sourced before this file runs
# shellcheck disable=SC2034  # $fail is the caller's accumulator in smoke.sh; assigning it here is the whole point

# Dual-core ITM-verdict apps (see $dualcore_itm_apps): boot the single M85 .elf
# (its Cortex-M33 image rides inside as a .cpu1_image PT_LOAD, so board_sim
# spins up the second engine automatically). BOARD_SIM_STOP_ON ends the run at
# the app's persistent steady-state ITM line; then assert the strong verdict --
# which only the M33 executing can produce -- in the captured output.
smoke_verdict_dualcore_itm() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# EPUB-import apps (see $import_reader_apps): attach the baked EPUB card and
# assert the app's full PASS banner. BOARD_SIM_STOP_ON ends the run the moment
# the banner prints (the app idles in WFI afterwards). A generous chunk budget
# covers the one-time compile; a missing card image (mkfontimg/Python absent)
# surfaces here as a banner miss, which is the correct loud failure.
smoke_verdict_import_reader() {
  local app="$1"
  want="$(uart_expect "$app")"
  if [ -z "$book_sd_image" ]; then
    echo "NO CARD (build_book_sd_image failed -- mkfontimg/python3 missing?)"
    fail=1
    return 0
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# I2C/I3C target-mode apps (see $i3c_target_apps): the firmware is the
# addressed peripheral and board_sim is the external controller. Bound the run
# (the demo idles forever) and assert board_sim's target summary -- a non-zero
# accepted-transaction count whose every firmware echo matched the byte the
# synthetic controller wrote (echo=Y). That proves the responder open + status
# + receive + send path end to end.
smoke_verdict_i3c_target() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# USB device-enumeration apps: the virtual host drives chapter-9; assert the
# device reaches CONFIGURED. BOARD_SIM_USB_STOP stops the run a short settle
# window after enumeration completes -- these apps never idle (HID jiggles its
# mouse, MSC keeps answering polls), so this is what keeps the gate fast and
# deterministic. MAX_CHUNKS is the failure cap (no CONFIGURED -> run ends ->
# ENUM FAIL); the WALL_S floor stops the wall-clock guard truncating first.
smoke_verdict_usb_enum() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

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
smoke_verdict_usb_msc_sd() {
  local app="$1"
  if [ -z "$msc_sd_image" ]; then
    echo "NO CARD (build_msc_sd_image failed -- mkbookimg unavailable?)"
    fail=1
    return 0
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# USB host-mode apps: the firmware's host stack enumerates board_sim's virtual
# device and drives it end to end. Three classes are covered: a HID boot
# keyboard (reports decode to "RA8D2"), a read-only FAT16 MSC disk (mount +
# browse + content-verify 1 MiB vs MRAM + write-reject), and a read-WRITE
# FAT16 disk (usb_host_file_ops: create / read-back / rename / unlink a file).
# BOARD_SIM_STOP_ON stops the run as soon as the success line prints (these
# apps loop forever otherwise). Assert the app's own PASS banner.
smoke_verdict_usb_host() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# microSD FORMAT apps: attach a blank 64 MiB card with --sd-new (the app
# reformats it as FAT12/FAT16/FAT32 in turn), then assert the app's own
# PASS banner. 64 MiB is large enough that the formatter's auto cluster-size
# sweep lands every type in its valid band. The app loops forever after the
# banner, so BOARD_SIM_STOP_ON ends the run as soon as ALL PASS prints.
smoke_verdict_sd_format() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# microSD ra8_io apps: blank FAT16 card via --sd-new, assert the app banner.
smoke_verdict_sd_io() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# OSPI-NOR ra8_io app: no card needed (OSPI is modelled internally), but the
# erase-reprogram RMW is slow -- STOP_ON ends the run as soon as PASS prints.
smoke_verdict_xspi_io() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# IT8951 e-paper app (see $eink_apps): attach the modelled controller with
# --eink, let epaper_refresh drive the full ra8_epaper -> display-PAL e-ink
# path (init, full refresh, partial refresh), and assert its PASS banner.
# STOP_ON ends the run at the banner (the app heartbeats forever after); the
# per-pixel SPI streaming needs a bigger chunk budget than the default. The SD
# apps show SPI-bus noise can carry NULs, so strip non-printables before bash.
smoke_verdict_eink() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# Cellular AT-modem app (see $modem_apps): attach the modelled AT modem on
# SCI7 with --modem, let modem_at_demo drive the full ra8_modem_at -> ra8_sci
# state machine (sync, SIM, signal, registration + URC, PS attach, +CME ERROR
# path), and assert its PASS banner. STOP_ON ends the run at the banner (the
# app heartbeats forever after). Without --modem the modem never answers and
# the app reports "modem: sync FAIL", so the model is load-bearing (SIM==HIL).
smoke_verdict_modem() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# Deep-idle self-parking app (see $selfpark_banner_apps): stop at the PASS
# banner so the deliberate lpi_panic_halt low-power park is not misread as a
# fault give-up by pc_in_halt_loop(), then assert the banner. Normal 300 s wall
# guard, single run -- this is NOT the periodic-tick WALL_S=0 path.
smoke_verdict_selfpark_banner() {
  local app="$1"
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
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# Verdict for the button class (see $button_apps in smoke_apps.sh).
smoke_verdict_button() {
  local app="$1"
  # The app ran with --button held (see sim_extra_args). gpio_input_demo
  # mirrors SW1 -> LED1, so the injected press must light LED1.
  if grep -qE "LED1[^]]*ON" <<<"$out"; then
    echo "OK (pc=$pc, --button -> LED1 ON)"
  else
    echo "BUTTON NO-OP (pc=$pc; --button did not light LED1)"
    fail=1
  fi
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# Verdict for the render assert class (see $render_assert_apps in smoke_apps.sh).
smoke_verdict_render_assert() {
  local app="$1"
  ppm="$(mktemp)"
  "$sim" "$elf" --ppm "$ppm" ${extra[@]+"${extra[@]}"} >/dev/null 2>&1 || true
  colors="$(count_ppm_colors "$ppm")"
  rm -f "$ppm"
  if [ "${colors:-0}" -lt "$min_render_colors" ]; then
    echo "RENDER SPARSE (pc=$pc, $colors colors < $min_render_colors)"
    fail=1
    return 0
  fi
  case " $golden_apps " in
    *" $app "*)
      if python3 "$ROOT/scripts/gen/ereader_golden.py" check \
        --elf "$elf" --board-sim "$sim" --golden-dir "$golden_dir" \
        --out-dir /tmp/ereader_golden_out >"/tmp/golden_$app.log" 2>&1; then
        echo "OK (pc=$pc, render=$colors colors, golden OK)"
      else
        echo "GOLDEN DRIFT (pc=$pc; see /tmp/golden_$app.log)"
        fail=1
      fi
      return 0
      ;;
  esac
  echo "OK (pc=$pc, render=$colors colors)"
  # Explicit: this helper's result is the line it printed, never $?.
  # A trailing conditional's status leaking out here is what aborted
  # the caller mid-app and printed a name with no verdict.
  return 0
}

# Build one app and locate its ELF. Sets the global $elf and $extra; assigns
# $fail and returns 1 when the app cannot be run at all.
smoke_build_app() {
  local app="$1"
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
    return 1
  fi
  elf="$(find examples -path "*/$app/build/$app.elf" 2>/dev/null | head -1)"
  if [ -z "$elf" ]; then
    echo "NO ELF"
    fail=1
    return 1
  fi
  # sim_extra_args prints zero or more whitespace-separated flags; read them
  # into an array so each flag becomes exactly one argv entry instead of
  # relying on word-splitting. The ${extra[@]+...} guard at each use site
  # keeps an empty array from tripping `set -u` under bash 3.2 (macOS).
  return 0
}

# Route one app to its class verdict. Returns 0 when a class owned the run
# and has already printed its result, 1 when no class claims the app and the
# generic assertions below must judge it.
#: class-list variable -> the function that owns that class's verdict.
#: Order is the order the original chain tested in; the first match wins.
SMOKE_CLASS_VERDICTS=(
  "dualcore_itm_apps:smoke_verdict_dualcore_itm"
  "import_reader_apps:smoke_verdict_import_reader"
  "i3c_target_apps:smoke_verdict_i3c_target"
  "usb_enum_apps:smoke_verdict_usb_enum"
  "usb_msc_sd_apps:smoke_verdict_usb_msc_sd"
  "usb_host_apps:smoke_verdict_usb_host"
  "sd_format_apps:smoke_verdict_sd_format"
  "sd_io_apps:smoke_verdict_sd_io"
  "xspi_io_apps:smoke_verdict_xspi_io"
  "eink_apps:smoke_verdict_eink"
  "modem_apps:smoke_verdict_modem"
  "selfpark_banner_apps:smoke_verdict_selfpark_banner"
)

# Route one app to its class verdict. Returns 0 when a class owned the run
# and has already printed its result, 1 when no class claims the app and the
# generic assertions must judge it.
smoke_dispatch_class() {
  local app="$1" entry cls fn
  for entry in "${SMOKE_CLASS_VERDICTS[@]}"; do
    cls="${entry%%:*}"
    fn="${entry##*:}"
    case " ${!cls} " in
      *" $app "*)
        "$fn" "$app"
        return 0
        ;;
    esac
  done
  return 1
}

# The assertions every unclassified app is held to: no fault, no BKPT, the run
# budget reached, the final PC outside any halt loop, and the expected banner.
# Run an unclassified app on board_sim and capture its output into $out.
#
# Every app with a known banner runs with BOARD_SIM_STOP_ON set and the
# CPU-time wall guard DISABLED, so the run is bounded only by the
# deterministic instruction-counted chunk budget. An app with no banner has
# no stop condition to key on and keeps the 300 s guard.
smoke_capture_run() {
  local app="$1"
  # Run the app on board_sim. EVERY app with a known banner runs with the
  # CPU-time wall-clock guard DISABLED (BOARD_SIM_WALL_S=0), so the run is
  # bounded only by the deterministic instruction-counted chunk budget and never
  # by host CPU-time. BOARD_SIM_STOP_ON ends the run the instant the banner
  # prints, so disabling the guard cannot let a genuinely-stuck app run long:
  # such an app is still bounded by k_run_max_chunks (40000), deterministically.
  #
  # #168 diagnosed this and fixed it for $periodic_tick_apps only. The root
  # cause is not specific to those three apps: board_sim's guard is CPU-time
  # (clock()), so under host load Unicorn's TCG re-translation burns the budget
  # faster than wall time and TRUNCATES the run before the (deterministic)
  # banner prints. Any app on the WALL_S>0 path can therefore fail as
  # "DID NOT REACH THE RUN BUDGET" purely because the box was busy -- which is
  # exactly what dtc_transfer_demo did on c43ba074a while three agents loaded
  # the runner host, passing on the runs either side of it with an identical
  # binary. 57 of the 60 banner-carrying apps were still on that path.
  #
  # An app with no banner has no stop condition to key on, so it keeps the 300 s
  # guard; its chunk budget still bounds it.
  tick_want="$(uart_expect "$app")"
  tick_tries=1
  tick_stop="$tick_want"
  case " $periodic_tick_apps " in
    *" $app "*)
      # Free-running timer polls: retry stays as cheap insurance only.
      tick_tries=3
      ;;
  esac
  for _t in $(seq 1 "$tick_tries"); do
    if [ -n "$tick_stop" ]; then
      out="$(BOARD_SIM_STOP_ON="$tick_stop" BOARD_SIM_WALL_S=0 "$sim" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
    else
      out="$(BOARD_SIM_WALL_S=300 "$sim" "$elf" ${extra[@]+"${extra[@]}"} 2>&1 || true)"
    fi
    grep -qF "$tick_want" <<<"$out" && break
  done
  # The loop's status is the last `grep && break`, i.e. "did the banner match
  # on the final attempt". That is NOT this function's result -- capturing a
  # run always succeeds; judging it is smoke_assert_run's job. Returning the
  # grep status here made `set -e` abort the caller mid-line, printing an app
  # name with no verdict. Never let a conditional leak out as a return value.
  return 0
}

# Assert the app printed its expected SCI banner. Returns 0 when the app HAS a
# banner (pass or fail is already reported), 1 when it has none and the caller
# should fall through to the bare pc-only verdict.
smoke_assert_banner() {
  local app="$1"
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
    return 0
  fi
  return 1
}

# Assert one captured run: no fault, no BKPT, the run budget reached, the
# final PC outside every halt loop, and then the class-specific pixel /
# button / banner checks that only make sense once the run is known good.
smoke_assert_run() {
  local app="$1"
  if grep -q "INVALID INSN\|UNMAPPED" <<<"$out"; then
    echo "FAULT (invalid opcode / unmapped access)"
    fail=1
    return 0
  fi
  # A firmware BKPT (Default_Handler's bkpt, a failed assert, a fault give-up)
  # is a halt regardless of which symbol it sits in -- board_sim stops on it
  # and prints this line. Catch it directly so a bare assert outside the named
  # *panic_halt / *_halt_loop symbols pc_in_halt_loop() knows about still fails.
  if grep -q "executed a BKPT" <<<"$out"; then
    echo "BKPT HALT ($(echo "$out" | sed -n 's/.*executed a BKPT @ *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1))"
    fail=1
    return 0
  fi
  if ! grep -q "EXECUTED to the run budget" <<<"$out"; then
    echo "DID NOT REACH THE RUN BUDGET"
    fail=1
    return 0
  fi
  pc="$(echo "$out" | sed -n 's/.*final PC *: *\(0x[0-9A-Fa-f]*\).*/\1/p' | head -1)"
  pcval=$((pc))
  if pc_in_halt_loop "$elf" "$pcval"; then
    echo "PANIC-HALT (pc=$pc)"
    fail=1
    return 0
  fi
  case " $button_apps " in
    *" $app "*)
      smoke_verdict_button "$app"
      return 0
      ;;
  esac
  case " $render_assert_apps " in
    *" $app "*)
      smoke_verdict_render_assert "$app"
      return 0
      ;;
  esac
  smoke_assert_banner "$app" && return 0
  echo "OK (pc=$pc)"
}

# The assertions every unclassified app is held to.
smoke_run_generic() {
  local app="$1"
  smoke_capture_run "$app"
  smoke_assert_run "$app"
}

# Build one app, then let its class -- or the generic assertions -- judge it.
smoke_run_app() {
  local app="$1"
  smoke_build_app "$app" || return 0
  smoke_dispatch_class "$app" && return 0
  smoke_run_generic "$app"
}
