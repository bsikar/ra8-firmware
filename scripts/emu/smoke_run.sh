#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/emu/smoke_run.sh -- run ONE app and assert its result.
#
# SOURCED, NEVER EXECUTED. scripts/emu/smoke.sh is the only entry point; it
# sets ROOT / emu_dir / emu before sourcing, and owns the app list, the build
# phase and the summary.
#
# shellcheck disable=SC2154  # lists and ROOT/emu/elf/fail come from smoke_apps.sh and smoke.sh
# shellcheck disable=SC2034  # $fail is the caller's accumulator in smoke.sh

readonly SMOKE_FAULT_RE='zig core: (unknown encoding|EPSR\.T clear|bus fault|invalid exception return|unaligned MemA access|stack limit overrun|no vector table)'
readonly SMOKE_BKPT_RE='zig core: BKPT halted'
readonly SMOKE_ANY_FAULT_RE='zig core: (unknown encoding|EPSR\.T clear|bus fault|invalid exception return|BKPT halted|unaligned MemA access|stack limit overrun|no vector table)'
readonly SMOKE_CLEAN_RE='zig core: ran [0-9]+ instructions clean|stopped clean '

smoke_core_args() { # primary-elf -> --ns / --cpu1 flags into caller $core_args
  local elf="$1" ns="${1%.elf}_ns.elf" cpu1="${1%.elf}_cpu1.elf"
  core_args=()
  [ -f "$ns" ] && core_args+=(--ns "$ns")
  [ -f "$cpu1" ] && core_args+=(--cpu1 "$cpu1")
  return 0
}

# Dual-core ITM-verdict apps (see $dualcore_itm_apps): boot the M85 ELF with
# `--cpu1 <app>_cpu1.elf`, stop via --until on its steady-state console banner,
# and assert the CPU1 ITM verdict (`itm: ...`).
smoke_verdict_dualcore_itm() {
  local app="$1" dc_stop dc_want dout
  dc_stop="$(dualcore_stop "$app")"
  dc_want="$(dualcore_assert "$app")"
  smoke_core_args "$elf"
  dout="$("$emu" "$elf" --console --until "$dc_stop" --ms 5000 \
    ${core_args[@]+"${core_args[@]}"} ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$dout"; then
    echo "FAULT (during dual-core run)"
    fail=1
  elif grep -qF "$dc_want" <<<"$dout"; then
    echo "OK (dual-core ITM verdict: '$dc_want')"
  else
    echo "DUAL-CORE FAIL (no ITM verdict '$dc_want' -- M33 did not run?)"
    fail=1
  fi
  return 0
}

# EPUB-import apps (see $import_reader_apps).
smoke_verdict_import_reader() {
  local app="$1" want iout
  want="$(uart_expect "$app")"
  if [ -z "$book_sd_image" ]; then
    echo "NO CARD (build_book_sd_image failed -- mkfontimg/python3 missing?)"
    fail=1
    return 0
  fi
  iout="$("$emu" "$elf" --sd "$book_sd_image" --console --until "$want" \
    --instructions 40000000 </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$iout"; then
    echo "FAULT (during EPUB import)"
    fail=1
  elif grep -qF "$want" <<<"$iout"; then
    echo "OK (EPUB import: '$want')"
  else
    echo "IMPORT FAIL (did not reach the PASS banner)"
    fail=1
  fi
  return 0
}

# I2C/I3C target-mode apps (see $i3c_target_apps).
smoke_verdict_i3c_target() {
  local app="$1" tout xfers
  tout="$("$emu" "$elf" --instructions 4000000 --console </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$tout"; then
    echo "FAULT (during I2C target run)"
    fail=1
  elif grep -qE "I3C target:.*controller write\+read cycle\(s\), echo matched" <<<"$tout"; then
    xfers="$(sed -n 's/.*I3C target: own 0x[0-9A-Fa-f]*, \([0-9]*\) controller.*/\1/p' <<<"$tout" | head -1)"
    echo "OK (I2C target: ${xfers:-?} write+read cycle(s), echo matched)"
  else
    echo "I2C TARGET FAIL (no accepted transaction / echo mismatch)"
    fail=1
  fi
  return 0
}

# USB device-enumeration apps: chapter-9 enumeration on the USBFS jack.
smoke_verdict_usb_enum() {
  local app="$1" uout product
  uout="$("$emu" "$elf" --ms 1000 --console </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$uout"; then
    echo "FAULT (during USB bring-up)"
    fail=1
  elif grep -q "USBFS host: enumeration configured" <<<"$uout"; then
    product="$(sed -n 's/.*USBFS host: product "\(.*\)"/\1/p' <<<"$uout" | head -1)"
    echo "OK (USB enumerated -> configured${product:+, $product})"
  else
    echo "USB ENUM FAIL (device did not reach configured)"
    fail=1
  fi
  return 0
}

# Live-SD USB MSC device app (see $usb_msc_sd_apps).
smoke_verdict_usb_msc_sd() {
  local app="$1" mout
  if [ -z "$msc_sd_image" ]; then
    echo "NO CARD (build_msc_sd_image failed -- mkbookimg unavailable?)"
    fail=1
    return 0
  fi
  mout="$({ "$emu" "$elf" --sd "$msc_sd_image" --ms 1500 --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$mout"; then
    echo "FAULT (during live-SD MSC run)"
    fail=1
  elif grep -q "USBFS host: enumeration configured" <<<"$mout"; then
    echo "OK (USB MSC live-SD enumerated -> configured)"
  else
    echo "USB ENUM FAIL (device did not reach configured with SD card)"
    fail=1
  fi
  return 0
}

# USB host-mode apps (see $usb_host_apps).
smoke_verdict_usb_host() {
  local app="$1" hout banner
  hout="$("$emu" "$elf" --instructions 40000000 --until PASS --console </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$hout"; then
    echo "FAULT (during USB host bring-up)"
    fail=1
  elif grep -qE "USB HOST (KEYBOARD|MSC BROWSE) PASS|ALL FILE OPS PASSED" <<<"$hout"; then
    banner="$(grep -oE "USB HOST [A-Z ]*PASS|ALL FILE OPS PASSED" <<<"$hout" | head -1)"
    echo "OK ($banner)"
  else
    echo "USB HOST FAIL (host did not reach its PASS banner)"
    fail=1
  fi
  return 0
}

# microSD FORMAT apps: attach a blank 64 MiB FAT32 card via --sd-size / --sd-new.
smoke_verdict_sd_format() {
  local app="$1" fclean
  fclean="$({ "$emu" "$elf" --sd-size 64 --sd-new fat32 --until "ALL PASS" \
    --instructions 100000000 --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$fclean"; then
    echo "FAULT (during format/mount)"
    fail=1
  elif grep -q "FS FORMAT+MOUNT ALL PASS" <<<"$fclean"; then
    echo "OK (FS FORMAT+MOUNT ALL PASS -- FAT12/16/32 + exFAT)"
  else
    echo "FS FORMAT FAIL (did not reach ALL PASS banner)"
    fail=1
  fi
  return 0
}

# microSD ra8_io apps: blank 64 MiB FAT16 card via --sd-size / --sd-new.
smoke_verdict_sd_io() {
  local app="$1" want ioclean
  want="$(uart_expect "$app")"
  ioclean="$({ "$emu" "$elf" --sd-size 64 --sd-new fat16 --until "$want" \
    --instructions 40000000 --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$ioclean"; then
    echo "FAULT (during ra8_io SD round-trip)"
    fail=1
  elif grep -qF "$want" <<<"$ioclean"; then
    echo "OK (ra8_io SD backend: $want)"
  else
    echo "RA8_IO SD FAIL (did not reach the PASS banner)"
    fail=1
  fi
  return 0
}

# OSPI-NOR ra8_io app (OSPI modelled internally).
smoke_verdict_xspi_io() {
  local app="$1" want xsclean
  want="$(uart_expect "$app")"
  xsclean="$({ "$emu" "$elf" --until "$want" --instructions 80000000 \
    --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$xsclean"; then
    echo "FAULT (during ra8_io on-chip-NV round-trip)"
    fail=1
  elif grep -qF "$want" <<<"$xsclean"; then
    echo "OK (ra8_io on-chip-NV backend: $want)"
  else
    echo "RA8_IO on-chip-NV FAIL (did not reach the PASS banner)"
    fail=1
  fi
  return 0
}

# IT8951 e-paper app (e-ink panel is mounted on the default EK-RA8D2 board).
smoke_verdict_eink() {
  local app="$1" want epclean
  want="$(uart_expect "$app")"
  epclean="$({ "$emu" "$elf" --until "$want" --ms 3000 \
    --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$epclean"; then
    echo "FAULT (during e-paper refresh)"
    fail=1
  elif grep -qF "$want" <<<"$epclean"; then
    echo "OK (IT8951 e-paper: $want)"
  else
    echo "EINK FAIL (did not reach the PASS banner)"
    fail=1
  fi
  return 0
}

# Cellular AT-modem app (AT modem on SCI7 is mounted on the default board).
smoke_verdict_modem() {
  local app="$1" want mdclean
  want="$(uart_expect "$app")"
  mdclean="$({ "$emu" "$elf" --until "$want" --ms 3000 \
    --console </dev/null 2>&1 || true; } | LC_ALL=C tr -cd '[:print:]\n')"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$mdclean"; then
    echo "FAULT (during AT modem run)"
    fail=1
  elif grep -qF "$want" <<<"$mdclean"; then
    echo "OK (AT modem: $want)"
  else
    echo "MODEM FAIL (did not reach the PASS banner)"
    fail=1
  fi
  return 0
}

# Deep-idle self-parking apps (stop via --until before lpi_panic_halt).
smoke_verdict_selfpark_banner() {
  local app="$1" want spout
  want="$(uart_expect "$app")"
  spout="$("$emu" "$elf" --until "$want" --ms 5000 --console \
    ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
  if grep -qE "$SMOKE_ANY_FAULT_RE" <<<"$spout"; then
    echo "FAULT (during deep-idle run)"
    fail=1
  elif grep -qF "$want" <<<"$spout"; then
    echo "OK (deep-idle self-park, uart: '$want')"
  else
    echo "DEEP-IDLE FAIL (did not reach the '$want' banner)"
    fail=1
  fi
  return 0
}

# Verdict for the button class (see $button_apps in smoke_apps.sh).
smoke_verdict_button() {
  local app="$1"
  if grep -qE "GPIO LEDs:.*LED1[^]]*ON" <<<"$out"; then
    echo "OK (pc=$pc, sw1 -> LED1 ON)"
  else
    echo "BUTTON NO-OP (pc=$pc; sw1 did not light LED1)"
    fail=1
  fi
  return 0
}

# Verdict for the render assert class (see $render_assert_apps in smoke_apps.sh).
smoke_verdict_render_assert() {
  local app="$1" frames_dir last_ppm colors
  frames_dir="$(mktemp -d)"
  "$emu" "$elf" --ms 2000 --frames-out "$frames_dir" \
    ${extra[@]+"${extra[@]}"} </dev/null >/dev/null 2>&1 || true
  last_ppm="$(find "$frames_dir" -maxdepth 1 -name 'frame_*.ppm' | sort | tail -n 1)"
  colors=0
  [ -n "$last_ppm" ] && [ -f "$last_ppm" ] && colors="$(count_ppm_colors "$last_ppm")"
  rm -rf "$frames_dir"
  if [ "${colors:-0}" -lt "$min_render_colors" ]; then
    echo "RENDER SPARSE (pc=$pc, $colors colors < $min_render_colors)"
    fail=1
    return 0
  fi
  case " $golden_apps " in
    *" $app "*)
      if python3 "$ROOT/scripts/gen/ereader_golden.py" check \
        --elf "$elf" --emulator "$emu" --golden-dir "$golden_dir" \
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
  return 0
}

smoke_app_dir() {
  python3 scripts/dev/ra8_apps.py dir "$1"
}

# Build one app and locate its ELF. Sets $elf and $extra.
smoke_build_app() {
  local app="$1"
  local app_dir=""
  printf '  %-24s ' "$app"
  if ! app_dir="$(smoke_app_dir "$app")" || [ -z "$app_dir" ]; then
    echo "APP NOT FOUND"
    fail=1
    return 1
  fi
  app_build=(/bin/bash -p "$ROOT/scripts/dev/run_just.sh" apps::build "$app")
  case " $dualcore_itm_apps " in
    *" $app "*)
      rm -rf "$app_dir/build"
      app_build=(/bin/bash -p "$ROOT/scripts/dev/run_just.sh" apps::build "$app" Debug)
      ;;
  esac
  if ! "${app_build[@]}" >"/tmp/smoke_build_$app.log" 2>&1; then
    echo "BUILD FAIL (see /tmp/smoke_build_$app.log)"
    fail=1
    return 1
  fi
  elf="$app_dir/build/$app.elf"
  if [ ! -f "$elf" ]; then
    echo "NO ELF"
    fail=1
    return 1
  fi
  extra=()
  read -r -a extra <<<"$(emu_extra_args "$app")"
  return 0
}

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

# Capture one generic run of ra8_emulator into $out.
smoke_capture_run() {
  local app="$1" tick_want tick_tries tick_stop _t
  smoke_core_args "$elf"
  tick_want="$(uart_expect "$app")"
  tick_tries=1
  tick_stop="$tick_want"
  case " $periodic_tick_apps " in
    *" $app "*) tick_tries=3 ;;
  esac
  for _t in $(seq 1 "$tick_tries"); do
    if [ -n "$tick_stop" ]; then
      out="$("$emu" "$elf" --console --until "$tick_stop" --ms 5000 \
        ${core_args[@]+"${core_args[@]}"} ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    else
      out="$("$emu" "$elf" --console --ms 1000 \
        ${core_args[@]+"${core_args[@]}"} ${extra[@]+"${extra[@]}"} </dev/null 2>&1 || true)"
    fi
    grep -qF "$tick_want" <<<"$out" && break
  done
  return 0
}

smoke_assert_banner() {
  local app="$1" want
  want="$(uart_expect "$app")"
  if [ -n "$want" ]; then
    if grep -qF "$want" <<<"$out"; then
      echo "OK (pc=$pc, uart: '$want')"
    else
      echo "UART MISMATCH (pc=$pc; expected '$want' in the SCI output)"
      fail=1
    fi
    return 0
  fi
  return 1
}

smoke_assert_run() {
  local app="$1" pcval
  if grep -qE "$SMOKE_FAULT_RE" <<<"$out"; then
    echo "FAULT (invalid opcode / bus fault / core stop)"
    fail=1
    return 0
  fi
  if grep -qE "$SMOKE_BKPT_RE" <<<"$out"; then
    echo "BKPT HALT ($(sed -n 's/.*zig core: BKPT halted at \(0x[0-9A-Fa-f]*\).*/\1/p' <<<"$out" | head -1))"
    fail=1
    return 0
  fi
  if ! grep -qE "$SMOKE_CLEAN_RE" <<<"$out"; then
    echo "DID NOT REACH CLEAN COMPLETION"
    fail=1
    return 0
  fi
  pc="$(sed -n 's/.*\(zig core: ran [0-9]* instructions clean\|stopped clean [^,]*\), pc \(0x[0-9A-Fa-f]*\).*/\2/p' <<<"$out" | head -1)"
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

smoke_run_generic() {
  local app="$1"
  smoke_capture_run "$app"
  smoke_assert_run "$app"
}

smoke_run_app() {
  local app="$1"
  smoke_build_app "$app" || return 0
  smoke_dispatch_class "$app" && return 0
  smoke_run_generic "$app"
}
