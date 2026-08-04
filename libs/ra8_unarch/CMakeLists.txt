# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_unarch -- bounded, fail-closed unarchivers for untrusted content.
#
# Domain layer that decodes the wrapped / container formats the e-reader
# meets on an SD card, every one of them charged against the unified
# decompression-limits policy in libs/ra8_core (ra8_decomp_limits.h):
#
#   inc/ra8_unarch_io.h        shared seek+read seam + flat-memory backing
#   inc/ra8_unarch_tar.h       clean-room streaming ustar/pax walker (.cbt,
#                              .tar.gz, .tar.xz)
#   inc/ra8_unarch_gzip.h      clean-room gzip member decoder (RFC 1952) over
#                              the vendored miniz DEFLATE core
#   inc/ra8_unarch_xz.h        bounded XZ/LZMA2 session + unwrap over the
#                              vendored xz-embedded (SOUP)
#   inc/ra8_unarch_xz_pool.h   zero-heap bump arena behind xz-embedded's
#                              allocator seam (NASA Rule 3)
#   inc/xz_config.h            first-party porting header the SOUP sources
#                              include (allocator, byte-order, mode selection)
#
# Vendored dependencies:
#   libs/third_party/xz_embedded/   XZ decoder (0BSD; see docs/SOUP/xz_embedded.md)
#   libs/third_party/miniz/         DEFLATE core (already vendored for ra8_epub)
#
# Dependency note: the gzip decoder needs the miniz implementation TU, which
# is supplied when an app also lists `ra8_epub` (or bare `miniz`) in `LIBS`.
# The xz_embedded TUs and include paths are wired automatically by
# cmake/ra8_add_app.cmake whenever an app pulls in ra8_unarch (directly or
# transitively through ra8_comic).
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly; app builds glob libs/ra8_unarch/src/*.c via ra8_add_app(LIBS ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
