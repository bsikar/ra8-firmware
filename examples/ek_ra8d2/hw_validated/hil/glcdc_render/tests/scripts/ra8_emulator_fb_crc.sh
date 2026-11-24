#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_emulator_fb_crc.sh -- ra8_emulator GLCDC framebuffer-CRC gate for glcdc_render.
#
# Boots glcdc_render on the board emulator (tools/ra8_emulator) and proves the
# modelled GLCDC display controller exposes the active graphics-layer
# framebuffer and that ra8_emulator composites the rendered pixels into its
# deterministic panel scan content hash.
#
# Three independent witnesses must verify the render:
#   1. the firmware's own banner over the SCI8 console
#        `glcdc-hil: layer1=ok dim=512x512 crc=<8hex> PASS`
#   2. the firmware's exported global g_glcdc_hil_ok (ra8_emulator --dump-sym),
#   3. the GLCDC model's end-of-run report, which DECODES the GR1 framebuffer
#      descriptor from the snooped register writes, composites the panel scan,
#      and checksums it:  `GLCDC: panel scanned, ..., content <8hex>`
#
# Witness 3 confirms the GLCDC GR1 layer is bound to the rendered buffer and
# that ra8_emulator composites it out of modelled memory independently of the
# firmware. Both hashes are deterministic across runs.
#
#   examples/ek_ra8d2/hw_validated/hil/glcdc_render/tests/scripts/ra8_emulator_fb_crc.sh
#
#
set -euo pipefail

# Force the C locale: ra8_emulator's run log can carry non-UTF-8 bytes (register
# dumps), which aborts BSD grep/sed pipelines under a UTF-8 locale.
export LC_ALL=C

# Golden FNV-1a-32 over the 512x512 RGB565 GR1 buffer (firmware hash) and
# over the composited 1024x600 RGB888 panel scan (ra8_emulator GLCDC report).
GOLDEN_CRC="B21B8D3D"
PANEL_GOLDEN_CRC="9D55CC75"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="$(git -C "$APP_DIR" rev-parse --show-toplevel)"
EMU_DIR="$ROOT/tools/ra8_emulator"
EMU="${RA8_EMULATOR:-$EMU_DIR/zig-out/bin/ra8_emulator}"
ELF="$APP_DIR/build/glcdc_render.elf"

fail() {
  echo "ra8_emulator_fb_crc: FAIL -- $1" >&2
  exit 1
}

# Build the emulator if it is not already present (via Zig).
if [ ! -x "$EMU" ]; then
  echo "ra8_emulator_fb_crc: building ra8_emulator ..."
  (cd "$EMU_DIR" && zig build -Doptimize=ReleaseFast) >/dev/null
fi
[ -x "$EMU" ] || fail "ra8_emulator did not build at $EMU"

# Build the firmware .elf if it is not already present.
if [ ! -f "$ELF" ]; then
  echo "ra8_emulator_fb_crc: building glcdc_render.elf ..."
  /bin/bash -p "$ROOT/scripts/dev/run_just.sh" --justfile "$ROOT/justfile" apps::build glcdc_render >/dev/null
fi
[ -f "$ELF" ] || fail "glcdc_render.elf did not build at $ELF"

# Pull an 8-hex checksum (`key=XXXXXXXX` or `key XXXXXXXX`) from a line matching tag.
hex_from() { # <logfile> <grep-tag> <key-regex> -> 8-hex uppercase hash on stdout
  grep -i "$2" "$1" | grep -oiE "${3}[0-9A-Fa-f]{8}" | head -1 | sed -E 's/.*([0-9A-Fa-f]{8})$/\1/' |
    tr '[:lower:]' '[:upper:]'
}

run_once() { # <run-tag> -> echoes "<panel_crc> <fw_crc> <ok>"
  local tag="$1" log
  log="$(mktemp "/tmp/glcdc_fbcrc_${tag}.log.XXXX")"
  "$EMU" "$ELF" --until PASS --ms 2000 --dump-sym g_glcdc_hil_ok --console </dev/null >"$log" 2>&1 || true
  local model_crc fw_crc ok
  model_crc="$(hex_from "$log" 'GLCDC: panel scanned' 'content[[:space:]]+')"
  fw_crc="$(hex_from "$log" 'glcdc-hil:.*PASS' 'crc=')"
  ok="$(grep -i 'g_glcdc_hil_ok' "$log" | grep -oE '= [0-9]+' | grep -oE '[0-9]+' | head -1)"
  rm -f "$log"
  echo "${model_crc:-NONE} ${fw_crc:-NONE} ${ok:-NONE}"
}

read -r M1 F1 OK1 < <(run_once a)
read -r M2 _ _ < <(run_once b)

echo "ra8_emulator_fb_crc: golden=$GOLDEN_CRC panel_golden=$PANEL_GOLDEN_CRC model=$M1 firmware=$F1 ok=$OK1 model_rerun=$M2"

[ "$OK1" = "1" ] || fail "firmware did not confirm the GLCDC layer (g_glcdc_hil_ok=$OK1)"
[ "$M1" = "$PANEL_GOLDEN_CRC" ] || fail "GLCDC panel scan hash $M1 != golden $PANEL_GOLDEN_CRC"
[ "$F1" = "$GOLDEN_CRC" ] || fail "firmware framebuffer CRC $F1 != golden $GOLDEN_CRC"
[ "$M1" = "$M2" ] || fail "GLCDC panel scan hash not deterministic ($M1 != $M2 across runs)"

echo "ra8_emulator_fb_crc: PASS -- GLCDC framebuffer scanned out of modelled RAM, fw=$GOLDEN_CRC panel=$PANEL_GOLDEN_CRC stable"
