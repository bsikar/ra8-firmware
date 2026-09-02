#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# emu_render_gate.sh -- ra8_emulator CRC gate for the ereader_m33 dual-core demo.
#
# Cross-builds the app (Debug, so the M85's INFO banner is compiled in), builds
# the ra8_emulator, boots the real M85 ELF (which auto-boots the embedded
# Cortex-M33 image), and asserts that the M33 rendered the held e-reader page
# into external SDRAM with the expected, deterministic CRC-32.
#
# What this proves: the M85 hands the reader to the M33; the M33 lays the opening
# page out with the production ra8_gfx text stack into an RGB565 framebuffer in
# modelled SDRAM (0x68000000), folds a CRC-32 over those pixels (reading them back
# out of SDRAM is itself the proof the render landed), and publishes base + dims +
# CRC through the shared SRAM mailbox. The M85 narrates the published CRC; this
# script asserts it equals the golden below and that no core faulted.
#
# Usage:
#   examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_render_gate.sh
#   ERM33_GOLDEN_CRC=C0BA74D3 \
#     examples/ek_ra8d2/hw_pending/ereader_m33/tests/scripts/emu_render_gate.sh
#
# Exit status: 0 = PASS (banner + golden CRC seen, no fault), 1 = FAIL.
#
#
set -euo pipefail

# Expected CRC-32 (uppercase hex) of the rendered RGB565 framebuffer. Stable
# across hosts because the render is integer-only and UB-free; re-baseline only
# if the page text, font, or framebuffer geometry deliberately changes.
GOLDEN_CRC="${ERM33_GOLDEN_CRC:-C0BA74D3}"

APP="ereader_m33"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app_dir="$(cd "${script_dir}/../.." && pwd)"
root="$(git -C "${app_dir}" rev-parse --show-toplevel)"
build_dir="${app_dir}/build"
emu_dir="${root}/tools/ra8_emulator"
toolchain="${root}/cmake/toolchain-ra8d2.cmake"
want="${APP}: rgb565 256x64 sdram crc=${GOLDEN_CRC} PASS"

echo "ereader_m33 gate: cross-building ${APP} (Debug) ..."
cmake -S "${app_dir}" -B "${build_dir}" \
  -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
cmake --build "${build_dir}" -j >/dev/null
elf="${build_dir}/${APP}.elf"
if [ ! -f "${elf}" ]; then
  echo "ereader_m33 gate: FAIL -- ${elf} not built" >&2
  exit 1
fi

echo "ereader_m33 gate: building ra8_emulator ..."
cmake -B "${emu_dir}/build" -S "${emu_dir}" >/dev/null
cmake --build "${emu_dir}/build" -j >/dev/null
emu="${emu_dir}/build/ra8_emulator"

echo "ereader_m33 gate: booting the M85 + M33 handoff on ra8_emulator ..."
# The render completes in well under 60 instruction-chunks; the bound below is a
# generous, runner-speed-independent ceiling (the M85 then parks in WFI). The
# banner reaches stdout over ITM, so the run is captured and grepped (ra8_emulator's
# RA8_EMU_STOP_ON only watches the SCI UART line, not ITM).
out="$(RA8_EMU_MAX_CHUNKS=1000 RA8_EMU_WALL_S=300 "${emu}" "${elf}" 2>&1 || true)"

if grep -qE 'INVALID INSN|UNMAPPED|executed a BKPT' <<<"${out}"; then
  echo "ereader_m33 gate: FAIL -- a core faulted during the handoff" >&2
  grep -E 'INVALID INSN|UNMAPPED|executed a BKPT' <<<"${out}" | head -3 >&2
  exit 1
fi

if grep -qF "${want}" <<<"${out}"; then
  echo "ereader_m33 gate: PASS -- ${want}"
  exit 0
fi

echo "ereader_m33 gate: FAIL -- expected banner not seen" >&2
echo "  want: ${want}" >&2
echo "  got : $(grep -oE "${APP}: rgb565[^\"]*" <<<"${out}" | head -1)" >&2
exit 1
