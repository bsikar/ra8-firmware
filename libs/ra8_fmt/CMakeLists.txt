# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_fmt -- Portable streaming format engines (JOF + RABOOK).
#
# Provides:
#   inc/ra8_fmt_io.h      format-neutral stream/sink/spool/transaction contract
#   inc/ra8_fmt_stream.h  JOF/RABOOK requirements, workspaces, and engine APIs
#   src/*.zig             Zig static archive (`libra8_fmt.a` via build.zig)
#
# Consumed by firmware apps listing `ra8_fmt` in `ra8_add_app(LIBS ...)` via
# `cmake/ra8_app/sources.cmake` and by host tools through the Zig build graph.
# Host POSIX fd/spool adapters stay under `tools/rabook_imagepack/`.

# Intentionally empty -- static archive is produced by Zig (`build.zig`).
