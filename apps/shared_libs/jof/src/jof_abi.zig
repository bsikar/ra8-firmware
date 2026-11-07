//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI `jof_produce.h`, `jof_internal.h` and the image-codec headers
//! publish, over the Zig implementation behind it.
//!
//! Scaffold slice (RA8FW-1139): the module exists and builds an archive the
//! CMake side can register, so the first ported function lands on a working
//! seam. Every jof C TU still compiles from JOF_SOURCES; as functions port
//! they are deleted from the C and exported from here, and core_hal.cmake
//! links this archive beside them (the ra8_fs pattern).
