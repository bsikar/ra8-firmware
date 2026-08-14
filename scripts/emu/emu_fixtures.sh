#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/emu/emu_fixtures.sh -- which ra8_emulator devices each app needs attached.
#
# SOURCED, NEVER EXECUTED. This is the ONE home for "app X needs a card": both
# scripts/emu/eil_all.sh and scripts/emu/matrix.sh source it, so an app that
# gains a storage dependency is taught to the harness once instead of twice.
#
# The second home is not hypothetical -- it is what this file was extracted to
# fix. eil_all.sh knew import_reader's neighbours needed a card; matrix.sh did
# not, so it booted import_reader with no card, the app printed
# "FAIL sd init" and deliberately trapped, and the matrix reported FAULT --
# "ra8_emulator model gap or firmware bug" -- for an app that was behaving exactly
# as designed and a harness that had withheld its device. A gate that
# manufactures a defect out of its own missing setup is worse than one that
# skips the app, because the number looks like evidence.
#
# Most apps only need a blank card: ra8_emulator's --sd-new builds one in-process
# (no external image), which is all the ra8_io / TrustZone / format / EPUB apps
# require -- they format or self-provision their own files. An app that must
# READ a pre-populated card gets an image baked once by the caller and passed
# in via the environment (EIL_FONT_IMG or RA8_EMU_IMPORT_READER_IMG).

# Build a microSD image carrying import_reader's deterministic EPUB fixture.
# The caller owns @p output and chooses its lifetime. The helper is shared by
# smoke.sh and matrix.sh so the deep assertion and breadth gate exercise the
# same exact card contents.
# shellcheck disable=SC2154 # ROOT is owned by each sourcing entry point.
emu_build_import_reader_card() { # <output.img> -> 0 on success
  local output="${1:?output image path required}"
  local fixture="$ROOT/examples/ek_ra8d2/hw_pending/import_reader/fixtures/book_src"
  local mk="$ROOT/tools/mkfontimg"
  local epub

  [ -d "$fixture" ] || return 1
  cmake -B "$mk/build" -S "$mk" >/dev/null 2>&1 || return 1
  cmake --build "$mk/build" >/dev/null 2>&1 || return 1
  epub="$(mktemp -t ra8_emulator_book.XXXXXX.epub)"
  if ! python3 - "$fixture" "$epub" <<'PY'; then
import sys, zipfile
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
epoch = (1980, 1, 1, 0, 0, 0)
with zipfile.ZipFile(out, "w") as zf:
    mimetype = zipfile.ZipInfo("mimetype", epoch)
    mimetype.compress_type = zipfile.ZIP_STORED
    zf.writestr(mimetype, b"application/epub+zip")
    for path in sorted(candidate for candidate in src.rglob("*") if candidate.is_file()):
        info = zipfile.ZipInfo(path.relative_to(src).as_posix(), epoch)
        info.compress_type = zipfile.ZIP_STORED
        zf.writestr(info, path.read_bytes())
PY
    rm -f "$epub"
    return 1
  fi
  if ! "$mk/build/mkfontimg" "$epub" "$output" BOOK.EPB >/dev/null 2>&1; then
    rm -f "$epub"
    return 1
  fi
  rm -f "$epub"
}

# Extra ra8_emulator arguments for one app.
#
# Emits nothing for the overwhelming majority of apps, which need no device.
# Callers expand the result inside an assignment under `set -e`, so this must
# exit 0 on every path -- a bare failing test at the end of a branch would
# abort the caller's worker.
emu_extra_args() { # <app> -> extra ra8_emulator args on stdout (may be empty)
  case "$1" in
    ra8_io_sd_demo | ra8_io_sdhi_demo | ra8_sdhi_card_demo | tz_secure_only_sd)
      # Format + round-trip a file on a blank FAT16 card.
      printf -- '--sd-new 64:fat16'
      ;;
    fs_format_mount | epub_open | epub_toc | pagecache)
      # These self-provision (books) or reformat (FAT12/16/32/exFAT) a blank
      # card themselves; a 64 MiB FAT32 --sd-new card is all ra8_emulator must
      # attach, per each app's own ra8_emulator recipe.
      printf -- '--sd-new 64:fat32'
      ;;
    usb_selftest_microsd)
      # USB self-loop that exposes the Pmod2 microSD as a read-only USB drive:
      # on real hardware a FAT card is inserted; in EIL ra8_emulator provisions a
      # blank FAT32 card so the host reads a valid MBR (0x55AA) + filesystem.
      printf -- '--sd-new 64:fat32'
      ;;
    import_reader)
      # Imports BOOK.EPB from a pre-populated card. A blank card reaches mount
      # and then fails at "FAIL import compile", so attaching --sd-new would
      # manufacture an app fault out of missing harness data. The parent bakes
      # the deterministic fixture once and exports its path to every worker.
      #
      # ereader_shelf / ereader_cover / ereader_comic are deliberately NOT here.
      # They were added alongside import_reader on the assumption that "reads a
      # library" implies "needs a card", and that guess broke ereader_shelf: it
      # serves books from baked MRAM and its asserted banner contains sd=0, so
      # attaching a blank card changed what it reported and failed the EIL gate.
      # Adding a device an app does not ask for is the same class of harness
      # error as withholding one it needs.
      [ -n "${RA8_EMU_IMPORT_READER_IMG:-}" ] &&
        printf -- '--sd %s' "$RA8_EMU_IMPORT_READER_IMG"
      ;;
    sd_font_render)
      # Reads FONT.OTF off the card (does not provision one), so it needs a
      # pre-populated image -- baked once by the parent into EIL_FONT_IMG. Empty
      # if the bake was skipped/failed; the app then fails with a clear reason.
      [ -n "${EIL_FONT_IMG:-}" ] && printf -- '--sd %s' "$EIL_FONT_IMG"
      ;;
    *) : ;;
  esac
  return 0
}
