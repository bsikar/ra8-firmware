//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_fs` archive: every ported unit, referenced so its C ABI
//! exports are emitted.

pub const c = @import("fs_c.zig").c;
pub const lock = @import("ra8_fs_lock_abi.zig");
pub const exfat_label = @import("ra8_fs_exfat_label_abi.zig");
pub const attr = @import("ra8_fs_attr_abi.zig");
pub const utime = @import("ra8_fs_utime_abi.zig");
pub const space = @import("ra8_fs_space_abi.zig");
pub const gpt = @import("ra8_fs_gpt_abi.zig");
pub const lfn = @import("ra8_fs_lfn_abi.zig");
pub const utf = @import("ra8_fs_utf_abi.zig");
pub const upcase = @import("ra8_fs_upcase_abi.zig");
pub const exfat_openw = @import("ra8_fs_exfat_openw_abi.zig");

comptime {
    _ = lock;
    _ = exfat_label;
    _ = attr;
    _ = utime;
    _ = space;
    _ = gpt;
    _ = lfn;
    _ = utf;
    _ = upcase;
    _ = exfat_openw;
}
