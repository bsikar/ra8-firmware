//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one `@cImport` of the ra8_fs C headers. Every Zig unit that touches a
//! C type (`ra8_fs_mount_t`, `dir_loc_t`, the exFAT set structs) or calls a C
//! walker imports this file, so they all share one set of translated types
//! and none of them hand-mirrors a C layout.

pub const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignas", "_Alignas");
    @cInclude("stdbool.h");
    @cInclude("ra8_fs_fat_internal.h");
    @cInclude("ra8_fs_meta.h");
});
