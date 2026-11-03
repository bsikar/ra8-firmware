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
pub const free_chain = @import("ra8_fs_free_chain_abi.zig");
pub const dirmk = @import("ra8_fs_dirmk_abi.zig");
pub const stat = @import("ra8_fs_stat_abi.zig");
pub const exfat_write = @import("ra8_fs_exfat_write_abi.zig");
pub const lfn_find = @import("ra8_fs_lfn_find_abi.zig");
pub const exfat_read = @import("ra8_fs_exfat_read_abi.zig");
pub const alloc = @import("ra8_fs_alloc_abi.zig");
pub const file = @import("ra8_fs_file_abi.zig");
pub const resolve = @import("ra8_fs_resolve_abi.zig");
pub const label = @import("ra8_fs_label_abi.zig");
pub const fat_core = @import("ra8_fs_fat_core_abi.zig");
pub const dir_run = @import("ra8_fs_dir_run_abi.zig");
pub const dir_erase = @import("ra8_fs_dir_erase_abi.zig");
pub const prim = @import("ra8_fs_fat_prim_abi.zig");
pub const fat_entry = @import("ra8_fs_fat_entry_abi.zig");

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
    _ = free_chain;
    _ = lfn_find;
    _ = exfat_read;
    _ = alloc;
    _ = file;
    _ = resolve;
    _ = label;
    _ = fat_core;
    _ = dir_run;
    _ = dir_erase;
    _ = prim;
    _ = fat_entry;
}
