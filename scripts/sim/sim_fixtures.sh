# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/sim/sim_fixtures.sh -- which board_sim devices each app needs attached.
#
# SOURCED, NEVER EXECUTED. This is the ONE home for "app X needs a card": both
# scripts/sim/sil_all.sh and scripts/sim/matrix.sh source it, so an app that
# gains a storage dependency is taught to the harness once instead of twice.
#
# The second home is not hypothetical -- it is what this file was extracted to
# fix. sil_all.sh knew import_reader's neighbours needed a card; matrix.sh did
# not, so it booted import_reader with no card, the app printed
# "FAIL sd init" and deliberately trapped, and the matrix reported FAULT --
# "board_sim model gap or firmware bug" -- for an app that was behaving exactly
# as designed and a harness that had withheld its device. A gate that
# manufactures a defect out of its own missing setup is worse than one that
# skips the app, because the number looks like evidence.
#
# Most apps only need a blank card: board_sim's --sd-new builds one in-process
# (no external image), which is all the ra8_io / TrustZone / format / EPUB apps
# require -- they format or self-provision their own files. An app that must
# READ a pre-populated card gets an image baked once by the caller and passed
# in via the environment (SIL_FONT_IMG).

# Extra board_sim arguments for one app.
#
# Emits nothing for the overwhelming majority of apps, which need no device.
# Callers expand the result inside an assignment under `set -e`, so this must
# exit 0 on every path -- a bare failing test at the end of a branch would
# abort the caller's worker.
sim_extra_args() { # <app> -> extra board_sim args on stdout (may be empty)
  case "$1" in
    ra8_io_sd_demo | ra8_io_sdhi_demo | ra8_sdhi_card_demo | tz_secure_only_sd)
      # Format + round-trip a file on a blank FAT16 card.
      printf -- '--sd-new 64:fat16'
      ;;
    fs_format_mount | epub_open | epub_toc | pagecache)
      # These self-provision (books) or reformat (FAT12/16/32/exFAT) a blank
      # card themselves; a 64 MiB FAT32 --sd-new card is all board_sim must
      # attach, per each app's own board_sim recipe.
      printf -- '--sd-new 64:fat32'
      ;;
    usb_selftest_microsd)
      # USB self-loop that exposes the Pmod2 microSD as a read-only USB drive:
      # on real hardware a FAT card is inserted; in SIL board_sim provisions a
      # blank FAT32 card so the host reads a valid MBR (0x55AA) + filesystem.
      printf -- '--sd-new 64:fat32'
      ;;
    import_reader | ereader_shelf | ereader_cover | ereader_comic)
      # Work against a library on the card. Without one import_reader asserts at
      # "FAIL sd init" and traps, which the breadth matrix recorded as a FAULT
      # against the APP rather than against the missing card. With a blank card
      # it reaches "card ready" and "volume mounted" and then fails at
      # "FAIL import compile" -- a real finding about the app, which is the
      # point: the harness has to supply the device before its verdict means
      # anything. A pre-populated library fixture (cf. sd_font_render's baked
      # FONT.OTF card) would take it further still.
      printf -- '--sd-new 64:fat32'
      ;;
    sd_font_render)
      # Reads FONT.OTF off the card (does not provision one), so it needs a
      # pre-populated image -- baked once by the parent into SIL_FONT_IMG. Empty
      # if the bake was skipped/failed; the app then fails with a clear reason.
      [ -n "${SIL_FONT_IMG:-}" ] && printf -- '--sd %s' "$SIL_FONT_IMG"
      ;;
    *) : ;;
  esac
  return 0
}
