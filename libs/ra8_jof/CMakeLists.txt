#
# libs/ra8_jof -- JOF band-tile atlas: format reader + import-time
# transcode producer (#231; the normalized image format shared with the
# longstrip scroll #289 and the codec policy #290).
#
# Layout:
#   inc/ra8_jof.h          format spec (authoritative), reader, memstore
#   inc/ra8_jof_produce.h  transcode producer (JPEG/PNG -> atlas)
#   src/ra8_jof.c              parse/validate + bounded per-tile decode
#   src/ra8_jof_produce.c      sniff/dispatch, band accumulator, tile encode
#   src/ra8_jof_png.c          streaming PNG pixel layer (tinfl inflate,
#                              unfilter, translate)
#   src/ra8_jof_png_chunk.c    streaming PNG chunk layer (prologue,
#                              PLTE/tRNS, ancillary walk)
#   src/ra8_jof_png_internal.h shared PNG decode state + prototypes
#   src/ra8_jof_internal.h     producer-private seams (bump arena, hooks)
#
# Dependencies: ra8_core (err/check/log), ra8_hal (the streaming JPEG
# decoder ra8_jpeg_sw_decode_stripes), ra8_io (ra8_io_compress/decompress),
# and the vendored miniz (tinfl, PNG IDAT inflate).
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/CMakeLists.txt and
# apps list `ra8_jof` in their ra8_add_app(... LIBS ...) (see the
# special-case block in cmake/ra8_add_app.cmake for the miniz/ra8_io wiring).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
