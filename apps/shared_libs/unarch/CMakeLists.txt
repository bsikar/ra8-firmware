# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/unarch -- bounded, fail-closed unarchivers for untrusted content.
#
# Domain layer that decodes the wrapped / container formats the e-reader
# meets on an SD card, every one of them charged against the unified
# decompression-limits policy in libs/ra8_core (ra8_decomp_limits.h):
#
#   inc/unarch_io.h        shared seek+read seam + flat-memory backing
#   inc/unarch_tar.h       clean-room streaming ustar/pax walker (.cbt,
#                              .tar.gz, .tar.xz)
#   inc/unarch_gzip.h      clean-room gzip member decoder (RFC 1952) over
#                              the vendored miniz DEFLATE core
#   inc/unarch_xz.h        bounded XZ/LZMA2 session + unwrap over the
#                              vendored xz-embedded (SOUP)
#   inc/unarch_xz_pool.h   zero-heap bump arena behind xz-embedded's
#                              allocator seam (NASA Rule 3)
#   inc/xz_config.h            first-party porting header the SOUP sources
#                              include (allocator, byte-order, mode selection)
#
# Vendored dependencies:
#   apps/shared_libs/third_party/xz_embedded/  XZ decoder (0BSD; see
#                                              docs/SOUP/xz_embedded.md)
#   apps/shared_libs/third_party/miniz/  DEFLATE core shared with epub
#
# Dependency note: the gzip decoder needs the miniz implementation TU, which
# is supplied when an app also lists `epub` (or bare `miniz`) in `LIBS`.
# The xz_embedded TUs and include paths are wired automatically by
# cmake/ra8_app/sources.cmake whenever an app pulls in unarch (directly or
# transitively through comic).
#
# Like the other domain libs in this tree, consumers reach into inc/ + src/
# directly; app builds glob apps/shared_libs/unarch/src/*.c via ra8_add_app(LIBS ...).
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.
