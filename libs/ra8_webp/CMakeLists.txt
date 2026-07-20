#
# libs/ra8_webp -- zero-heap WebP (VP8 / VP8L) decode facade over libwebp.
#
# Domain layer that decodes an in-memory WebP into a caller-owned RGBA8888
# buffer via the vendored libwebp decoder (libs/third_party/libwebp), routing
# every libwebp allocation through a caller-bound heap-free bump arena.
#
# Layout:
#   inc/ra8_webp.h            public decode facade (ra8_err_t)
#   inc/ra8_webp_arena.h      caller-bound bump arena hooks
#   src/ra8_webp.c            facade impl (the only TU that includes libwebp)
#   src/ra8_webp_arena.c      bump arena impl (WebPSafe{Malloc,Calloc,Free} sink)
#
# Vendored dependency (already at libs/third_party/libwebp/): the decode-only
# subset of libwebp 1.5.0. Its utils.c is patched (RA8 LOCAL PATCH, gated by
# -DRA8_WEBP_USE_ARENA) to call the ra8_webp arena instead of libc malloc; see
# docs/SOUP/libwebp.md.
#
# Like the other domain libs in this tree (ra8_epub, ra8_reflow), consumers
# reach into inc/ + src/ directly. The host unit test build in
# tests/CMakeLists.txt picks up ra8_webp via the recursive glob plus the
# libwebp third-party additions wired up there. An example app pulls it in with
# `LIBS ra8_webp`, which cmake/ra8_add_app.cmake special-cases to also compile
# the vendored libwebp decoder + define RA8_WEBP_USE_ARENA on its utils.c
# (mirroring the ra8_epub/miniz special-case).
#
# INTEGRATION(#290): the JOF tile producer (libs/ra8_jof) consumes this
# facade to normalise WebP manifest images to the band-tile format on import, so
# an app that lists ra8_jof in LIBS also compiles this facade + the
# vendored libwebp (see the ra8_jof block in cmake/ra8_add_app.cmake).
# NOTE(#289): the small-image ra8_reflow / ra8_img inline raster dispatch does
# not yet have a WebP arm; that lands with the longstrip render path.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
