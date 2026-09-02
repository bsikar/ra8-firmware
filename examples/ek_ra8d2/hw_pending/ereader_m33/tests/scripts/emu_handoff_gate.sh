#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# emu_handoff_gate.sh -- ra8_emulator gate for the #150 ereader_m33 MODE-SWITCH.
#
# Cross-builds the app (Debug, so the M85's INFO banners are compiled in), builds
# the ra8_emulator, boots the real M85 ELF (which auto-boots the embedded
# Cortex-M33 image), and drives the FULL park / wake / re-render handoff cycle:
#
#   M85 stages the book + arms the IPC0 wake + configures LPM -> releases M33 ->
#   M33 renders + holds page 0 -> M85 logs the page-0 verdict -> M85 PARKS
#   (CGC clock-gate + Sleep-mode WFI) -> M33 holds the page and, on each scripted
#   touch, bumps turn_req + pokes IPC0 -> M85 WAKES, does the heavy next-page
#   work, acks turn_ack, re-parks -> M33 re-renders (same deterministic CRC) and
#   sets turn_done -> repeated k_erm33_max_turns (3) times -> both cores park.
#
# What this proves, using ONLY existing ra8_emulator mechanisms (dual-core + IPC +
# WFI + SDRAM): the M85 really parks and is woken out of WFI by the M33's IPC
# poke; the M33 holds the page and re-renders on demand with a STABLE framebuffer
# CRC; and the cycle runs a fixed, deterministic number of times. The M85 narrates
# the whole exchange (ra8_emulator echoes only the primary core's ITM), so the gate
# asserts: (1) the page-0 render verdict, (2) one wake banner per page turn, and
# (3) the final handoff verdict carrying the re-render count + the stable CRC.
#
# Usage:
#   examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_handoff_gate.sh
#   ERM33_GOLDEN_CRC=C0BA74D3 \
#     examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_handoff_gate.sh
#
# Exit status: 0 = PASS (all banners seen, no fault), 1 = FAIL.
#
#
set -euo pipefail

# Expected CRC-32 (uppercase hex) of the rendered RGB565 framebuffer. Identical
# to the render gate's golden: the page content, font and geometry are unchanged,
# and the re-render is byte-for-byte the same, so the CRC stays stable across the
# whole handoff cycle. Re-baseline only if the page text/font/geometry changes.
GOLDEN_CRC="${ERM33_GOLDEN_CRC:-C0BA74D3}"

# Page-turn handoffs the M33 scripts (must match k_erm33_max_turns in
# ereader_m33.h). The M85 logs one wake banner per turn and the final verdict
# carries this count.
TURNS="${ERM33_TURNS:-3}"

APP="ereader_m33"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app_dir="$(cd "${script_dir}/../.." && pwd)"
root="$(git -C "${app_dir}" rev-parse --show-toplevel)"
build_dir="${app_dir}/build"
emu_dir="${root}/tools/ra8_emulator"
toolchain="${root}/cmake/toolchain-ra8d2.cmake"

page0_banner="${APP}: rgb565 256x64 sdram crc=${GOLDEN_CRC} PASS"
handoff_banner="${APP}: handoff turns=${TURNS} crc=${GOLDEN_CRC} PARKED"

echo "ereader_m33 handoff gate: cross-building ${APP} (Debug) ..."
cmake -S "${app_dir}" -B "${build_dir}" \
  -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
cmake --build "${build_dir}" -j >/dev/null
elf="${build_dir}/${APP}.elf"
if [ ! -f "${elf}" ]; then
  echo "ereader_m33 handoff gate: FAIL -- ${elf} not built" >&2
  exit 1
fi

echo "ereader_m33 handoff gate: building ra8_emulator ..."
cmake -B "${emu_dir}/build" -S "${emu_dir}" >/dev/null
cmake --build "${emu_dir}/build" -j >/dev/null
emu="${emu_dir}/build/ra8_emulator"

echo "ereader_m33 handoff gate: driving the M85-park / M33-hold / wake cycle ..."
# The cycle (boot + page-0 render + 3 park/wake/re-render turns) completes in a
# few dozen instruction-chunks; the bound below is a generous, runner-speed-
# independent ceiling. The M85 then parks in WFI for good. Banners reach stdout
# over ITM, so the run is captured and grepped.
out="$(RA8_EMU_MAX_CHUNKS=4000 RA8_EMU_WALL_S=300 "${emu}" "${elf}" 2>&1 || true)"

fail() {
  echo "ereader_m33 handoff gate: FAIL -- $1" >&2
  echo "----- ra8_emulator output (tail) -----" >&2
  tail -n 40 <<<"${out}" >&2
  exit 1
}

if grep -qE 'INVALID INSN|UNMAPPED|executed a BKPT' <<<"${out}"; then
  echo "ereader_m33 handoff gate: FAIL -- a core faulted during the handoff" >&2
  grep -E 'INVALID INSN|UNMAPPED|executed a BKPT' <<<"${out}" | head -3 >&2
  exit 1
fi

grep -qF "${page0_banner}" <<<"${out}" || fail "page-0 render verdict not seen (want: ${page0_banner})"

# One wake banner per scripted page turn -- proof the M85 really left WFI on each
# IPC poke (the mode-switch), not just once.
wakes="$(grep -cF 'woke from low-power for page turn' <<<"${out}" || true)"
if [ "${wakes}" != "${TURNS}" ]; then
  fail "expected ${TURNS} page-turn wakes, saw ${wakes}"
fi

grep -qF "${handoff_banner}" <<<"${out}" || fail "handoff verdict not seen (want: ${handoff_banner})"

echo "ereader_m33 handoff gate: PASS"
echo "  page-0 : ${page0_banner}"
echo "  wakes  : ${wakes} (one per scripted page turn)"
echo "  handoff: ${handoff_banner}"
exit 0
