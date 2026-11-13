# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/webp -- zero-heap WebP (VP8 / VP8L) decode facade over libwebp.
#
# Domain layer that decodes an in-memory WebP into a caller-owned RGBA8888
# buffer via the vendored libwebp decoder
# (apps/shared_libs/third_party/libwebp), routing
# every libwebp allocation through a caller-bound heap-free bump arena.
#
# Layout:
#   inc/ra8_webp.h            public decode facade (ra8_err_t)
#   inc/ra8_webp_arena.h      caller-bound bump arena hooks
#   build.zig                 the facade impl: src/ra8_webp_abi.zig (the only
#                             module that binds libwebp) plus the imgdec
#                             adapter, built and linked as the Zig `webp`
#                             archive (RA8FW-1209). No first-party C remains.
#
# Vendored dependency (at apps/shared_libs/third_party/libwebp/): the decode-only
# subset of libwebp 1.5.0. Its utils.c is patched (RA8 LOCAL PATCH, gated by
# -DRA8_WEBP_USE_ARENA) to call the ra8_webp arena instead of libc malloc; see
# docs/SOUP/libwebp.md. Those vendored TUs still compile on the C side and
# call the arena hooks this package's archive exports.
#
# Consumers reach into inc/ for the public headers and LINK the `webp` Zig
# archive. The app path registers it from this build.zig through
# cmake/ra8_app (`LIBS webp`, or transitively for jof, rabook_compile and
# reflow); the host test build declares it in tests/cmake/zig_libraries.cmake
# and links it in tests/cmake/zig_link_order.cmake; a standalone host tool
# gets it through ra8_webp_attach() in cmake/ra8_webp_vendor.cmake.
#
# INTEGRATION(normalize-on-import): the JOF tile producer (apps/shared_libs/jof) consumes this
# facade to normalise WebP manifest images to the band-tile format on import, so
# an app that lists jof in LIBS also links the webp archive + compiles the
# vendored libwebp (see the jof block in cmake/ra8_app/sources.cmake).
# INTEGRATION(inline WebP): the small-image reflow / ra8_img inline raster dispatch
# calls this facade too, so an app that lists reflow in LIBS links it as
# well (see the reflow block in cmake/ra8_app/sources.cmake).
#
#

# Intentionally empty -- consumers reach into inc/ directly and link the archive.
