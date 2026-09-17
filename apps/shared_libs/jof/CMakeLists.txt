# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/jof -- JOF band-tile atlas: format reader + import-time
# transcode producer (#231; the normalized image format shared with the
# longstrip scroll #289 and the codec policy #290).
#
# Layout:
#   inc/jof.h          format spec (authoritative), reader, memstore
#   inc/jof_audit.h    no-heap backing-agnostic structural audit
#   inc/jof_produce.h  transcode producer (JPEG/PNG -> atlas)
#   src/jof.c              parse/validate + bounded per-tile decode
#   src/jof_audit.c        coverage/geometry audit over injected reads
#   src/jof_produce.c      sniff/dispatch, band accumulator, tile encode
#   src/jof_png.c          streaming PNG pixel layer (tinfl inflate,
#                              unfilter, translate)
#   src/jof_png_chunk.c    streaming PNG chunk layer (prologue,
#                              PLTE/tRNS, ancillary walk)
#   src/jof_png_internal.h shared PNG decode state + prototypes
#   src/jof_internal.h     producer-private seams (bump arena, hooks)
#
# Dependencies: ra8_core (err/check/log), ra8_jpeg (the streaming software
# decoder ra8_jpeg_sw_decode_stripes), compress (ra8_compress/decompress),
# and the vendored miniz (tinfl, PNG IDAT inflate).
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly: the host unit-test build globs it in tests/CMakeLists.txt and
# apps list `jof` in their ra8_add_app(... LIBS ...) (see the
# special-case block in cmake/ra8_app/sources.cmake for the miniz/compress
# wiring).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
