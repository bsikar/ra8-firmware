#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_emulator_fb_crc.sh -- ra8_emulator GLCDC framebuffer-CRC gate for glcdc_render.
#
# Boots glcdc_render on the board emulator (tools/ra8_emulator) and proves the
# modelled GLCDC display controller (tools/ra8_emulator/src/periph/board_periph_glcdc.c)
# exposes the active graphics-layer framebuffer and that ra8_emulator reads the
# rendered pixels back out of modelled RAM and checksums them to a stable golden.
#
# Three independent witnesses must agree on the same FNV-1a-32 hash:
#   1. the firmware's own banner over the SCI8 console
#        `glcdc-hil: layer1=ok dim=512x512 crc=<8hex> PASS`
#   2. the firmware's exported global g_glcdc_hil_crc (ra8_emulator --dump-sym),
#   3. the GLCDC model's end-of-run report, which DECODES the GR1 framebuffer
#      descriptor (base / width / height / stride / format) from the snooped
#      register writes, then reads the framebuffer out of emulated memory and
#      hashes it:  `GLCDC : layer1 fb=0x........ 512x512 rgb565 ... crc=<8hex>`
#
# Witness 3 is the new coverage: it confirms the GLCDC GR1 layer is bound to the
# rendered buffer and that ra8_emulator can pull that buffer out of modelled memory
# independently of the firmware. The hash is deterministic (integer paint into a
# zeroed buffer), so a second run must reproduce it byte-for-byte.
#
#   examples/ek_ra8d2/hw_validated/hil/glcdc_render/tests/scripts/ra8_emulator_fb_crc.sh
#
#
set -euo pipefail

# Force the C locale: ra8_emulator's run log can carry non-UTF-8 bytes (register
# dumps), which aborts BSD grep/sed pipelines under a UTF-8 locale.
export LC_ALL=C

# The golden FNV-1a-32 over the 512x512 RGB565 render. Matches the on-bench HIL
# baseline (hil.conf HIL_EXPECT) and glcdc_render's gh_framebuffer_hash().
GOLDEN_CRC="B21B8D3D"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="$(git -C "$APP_DIR" rev-parse --show-toplevel)"
EMU_DIR="$ROOT/tools/ra8_emulator"
EMU="$EMU_DIR/build/ra8_emulator"
ELF="$APP_DIR/build/glcdc_render.elf"

fail() {
  echo "ra8_emulator_fb_crc: FAIL -- $1" >&2
  exit 1
}

# Build the emulator if it is not already present (auto-globs board_periph_*.c).
if [ ! -x "$EMU" ]; then
  echo "ra8_emulator_fb_crc: building ra8_emulator ..."
  cmake -S "$EMU_DIR" -B "$EMU_DIR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$EMU_DIR/build" -j >/dev/null
fi
[ -x "$EMU" ] || fail "ra8_emulator did not build at $EMU"

# Build the firmware .elf if it is not already present.
if [ ! -f "$ELF" ]; then
  echo "ra8_emulator_fb_crc: building glcdc_render.elf ..."
  /bin/bash -p "$ROOT/scripts/dev/run_just.sh" --justfile "$ROOT/justfile" apps::build glcdc_render >/dev/null
fi
[ -f "$ELF" ] || fail "glcdc_render.elf did not build at $ELF"

# Pull the 8-hex CRC out of a `... crc=XXXXXXXX ...` line matching a tag.
crc_from() { # <logfile> <grep-tag> -> 8-hex CRC on stdout (empty if absent)
  grep -i "$2" "$1" | grep -oiE 'crc=[0-9A-Fa-f]{8}' | head -1 | cut -d= -f2 |
    tr '[:lower:]' '[:upper:]'
}

run_once() { # <run-tag> -> echoes "<model_crc> <fw_crc> <ok>"
  local tag="$1" out err
  out="$(mktemp "/tmp/glcdc_fbcrc_${tag}.out.XXXX")"
  err="$(mktemp "/tmp/glcdc_fbcrc_${tag}.err.XXXX")"
  "$EMU" "$ELF" --dump-sym g_glcdc_hil_ok >"$out" 2>"$err" || true
  local model_crc fw_crc ok
  model_crc="$(crc_from "$err" 'GLCDC')"  # model report (stderr)
  fw_crc="$(crc_from "$out" 'glcdc-hil')" # firmware UART banner (stdout)
  ok="$(grep -i 'g_glcdc_hil_ok' "$err" | grep -oE '= [0-9]+' | grep -oE '[0-9]+' | head -1)"
  rm -f "$out" "$err"
  echo "${model_crc:-NONE} ${fw_crc:-NONE} ${ok:-NONE}"
}

read -r M1 F1 OK1 < <(run_once a)
read -r M2 _ _ < <(run_once b)

echo "ra8_emulator_fb_crc: golden=$GOLDEN_CRC model=$M1 firmware=$F1 ok=$OK1 model_rerun=$M2"

[ "$OK1" = "1" ] || fail "firmware did not confirm the GLCDC layer (g_glcdc_hil_ok=$OK1)"
[ "$M1" = "$GOLDEN_CRC" ] || fail "GLCDC model framebuffer CRC $M1 != golden $GOLDEN_CRC"
[ "$F1" = "$GOLDEN_CRC" ] || fail "firmware framebuffer CRC $F1 != golden $GOLDEN_CRC"
[ "$M1" = "$F1" ] || fail "model CRC $M1 disagrees with firmware CRC $F1"
[ "$M1" = "$M2" ] || fail "GLCDC framebuffer CRC not deterministic ($M1 != $M2 across runs)"

echo "ra8_emulator_fb_crc: PASS -- GLCDC framebuffer read out of modelled RAM, crc=$GOLDEN_CRC stable"
