# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/shared_libs/compress -- app/content compression over miniz.
#
# The module owns the heap-free DEFLATE facade and its optional VFS extension.
# It is intentionally outside libs/ra8_io: its production consumers are the
# JOF/RaBook content pipelines, while the core I/O fabric remains codec-free.
# Firmware consumers list `compress` and `miniz` in ra8_add_app(LIBS ...).
# Host tools include inc/ and compile the required src/ translation units.
