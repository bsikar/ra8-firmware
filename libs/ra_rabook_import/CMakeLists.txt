#
# libs/ra_rabook_import -- on-import EPUB -> .rabook compile-and-cache (#151).
#
# Makes the e-reader self-contained: drop a raw .epub on the SD card and the
# first open compiles it once into the flat RABOOK1 .rabook (via an injected
# compiler seam), caches the result on the card keyed by the source CRC-32, and
# takes the fast cached path on every subsequent open (read-through, no
# recompile). Crash-safe via temp+rename; the source .epub is never modified.
#
# No dynamic allocation (NASA Power-of-10 Rule 3): the manager streams the
# source through CRC-32 in a caller-sized scratch chunk and keeps no state but
# the cfg passed in. SOLID/DIP: the EPUB -> RABOOK1 compile is reached only
# through the injected ra_rabook_import_compile_fn seam and the filesystem only
# through ra_fs_mount_t, so the core manager depends on nothing but ra_fs +
# ra_core and is host-testable with a lightweight compile spy.
#
# Layout:
#   inc/ra_rabook_import.h            cache manager + import state machine API
#   inc/ra_rabook_import_compiler.h   production adapter onto the real compiler
#   src/ra_rabook_import.c            CRC key, freshness gate, temp+rename
#   src/ra_rabook_import_compiler.c   ra_epub_open_fs + ra_rabook_compile_from_epub
#
# The core manager TU (ra_rabook_import.c) depends only on ra_fs + ra_core. The
# adapter TU (ra_rabook_import_compiler.c) additionally pulls ra_epub +
# ra_rabook_compile and is only needed by apps that wire the real compiler.
#
# Header-and-source library; no standalone build target. Firmware apps list
# `ra_rabook_import` in ra_add_app(LIBS ...); the host test build folds the
# src/*.c into the shared object library the same way ra_rabook_compile is.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Header-and-source library; no standalone build target. See the note above.
