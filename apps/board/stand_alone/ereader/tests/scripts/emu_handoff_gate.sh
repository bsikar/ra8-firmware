#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# emu_handoff_gate.sh -- headless dual-core + TrustZone emulator gate for
# apps/board/stand_alone/ereader (RA8FW-250).
#
# Cross-builds ra8d2-ereader (Secure + Non-Secure ThreadX + CPU1 M33) with
# RA8_EREADER_LP_EMU_SOAK=ON so M85 parks in HOCO-stopped Sleep WFI after the
# initial page flush and wakes repeatedly on M33 IPC0 pokes across multiple
# page turns, asserting GLCDC frame flushes, zero faults, HOCO clock stop,
# and repeated M85<->M33 handoff transitions in ra8_emulator.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)"
APP_DIR="${ROOT}/apps/board/stand_alone/ereader"
BUILD_DIR="${1:-${APP_DIR}/build_emu_handoff}"
EMU="${RA8_EMULATOR:-${ROOT}/tools/ra8_emulator/zig-out/bin/ra8_emulator}"
TOOLCHAIN="${ROOT}/cmake/toolchain-ra8d2.cmake"

if [[ ! -x "${EMU}" ]] && command -v ra8_emulator >/dev/null 2>&1; then
  EMU="$(command -v ra8_emulator)"
fi

if [[ ! -x "${EMU}" ]]; then
  echo "ereader handoff gate: FAIL -- ra8_emulator not found" >&2
  exit 1
fi

cmake -S "${APP_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
  -DRA8_EREADER_LP_EMU_SOAK=ON >/dev/null
cmake --build "${BUILD_DIR}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
  --target ra8d2-ereader.elf ra8d2-ereader_ns.elf ra8d2-ereader_cpu1.elf

S_ELF="${BUILD_DIR}/ra8d2-ereader.elf"
NS_ELF="${BUILD_DIR}/ra8d2-ereader_ns.elf"
C1_ELF="${BUILD_DIR}/ra8d2-ereader_cpu1.elf"
python3 "${ROOT}/scripts/checks/check_sg_offsets.py" "${S_ELF}"

OUT="$("${EMU}" "${S_ELF}" --ns "${NS_ELF}" --cpu1 "${C1_ELF}" --ms "${ER_EMU_MS:-2500}" --console </dev/null 2>&1 || true)"

fail() {
  echo "ereader handoff gate: FAIL -- $1" >&2
  printf '%s\n' "${OUT}" | tail -n 80 >&2
  exit 1
}

printf '%s\n' "${OUT}" | grep -q "EXC] ERROR" && fail "CPU exception recorded"
printf '%s\n' "${OUT}" | grep -q "M33 secondary hold supervisor started" || fail "missing M33 start banner"
printf '%s\n' "${OUT}" | grep -q "Entering M33 hold" || fail "M85 never entered M33 hold"
printf '%s\n' "${OUT}" | grep -q "Backlight OFF -> requested M33 hold handoff" || fail "missing screen-off idle handoff request"

wake_count="$(printf '%s\n' "${OUT}" | grep -c "M85 wake -> page turn completed" || true)"
if [[ "${wake_count}" -lt 2 ]]; then
  fail "expected >= 2 repeated M85<->M33 page-turn wakes, observed ${wake_count}"
fi

echo "ereader handoff gate: PASS (${wake_count} repeated M85<->M33 wakes)"
