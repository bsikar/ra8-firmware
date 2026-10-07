//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one translation of the ra8_fs C headers: build.zig runs translate-c
//! over them and hands the result in as `fs_h`. Every Zig unit that touches a
//! C type (`ra8_fs_mount_t`, `dir_loc_t`, the exFAT set structs) or calls a C
//! walker imports this file, so they all share one set of translated types
//! and none of them hand-mirrors a C layout.

pub const c = @import("fs_h");
