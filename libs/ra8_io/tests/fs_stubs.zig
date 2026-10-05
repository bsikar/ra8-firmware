//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_fs facade the format registry (ra8_io_fsfmt_abi.zig) forwards to.
//! Every ra8_io test root links the whole archive, so each one pulls these in
//! with `comptime { _ = @import("fs_stubs.zig"); }`. By default every call
//! fails with k_ra8_err_not_supported and probe detects nothing; the knobs
//! below let the registry test drive probe and the directory walk.

const io = @import("ra8_io");
const fsfmt = io.fsfmt;

const not_supported: c_int = 0x107;

/// The type ra8_fs_probe reports, or null for a failing probe.
pub var probe_type: ?c_int = null;
/// Entries ra8_fs_dir_next hands out before reporting the end.
pub var dir_entries: u32 = 0;
/// ra8_fs_dir_close calls since the last reset.
pub var dir_closes: u32 = 0;

pub fn reset() void {
    probe_type = null;
    dir_entries = 0;
    dir_closes = 0;
}

export fn ra8_fs_probe(_: *const anyopaque, out: *c_int) c_int {
    out.* = probe_type orelse return not_supported;
    return 0;
}
export fn ra8_fs_mount(_: *const anyopaque, _: *?*anyopaque) c_int {
    return not_supported;
}
export fn ra8_fs_unmount(_: ?*anyopaque) c_int {
    return not_supported;
}
export fn ra8_fs_open(_: ?*anyopaque, _: [*:0]const u8, _: c_int, _: *?*anyopaque) c_int {
    return not_supported;
}
export fn ra8_fs_close(_: ?*anyopaque) c_int {
    return not_supported;
}
export fn ra8_fs_read(_: ?*anyopaque, _: *anyopaque, _: u32, _: *u32) c_int {
    return not_supported;
}
export fn ra8_fs_write(_: ?*anyopaque, _: *const anyopaque, _: u32) c_int {
    return not_supported;
}
export fn ra8_fs_seek(_: ?*anyopaque, _: u64) c_int {
    return not_supported;
}
export fn ra8_fs_tell(_: ?*const anyopaque, _: *u64) c_int {
    return not_supported;
}
export fn ra8_fs_size(_: ?*const anyopaque, _: *u64) c_int {
    return not_supported;
}
export fn ra8_fs_stat(_: ?*anyopaque, _: [*:0]const u8, _: *anyopaque) c_int {
    return not_supported;
}
export fn ra8_fs_dir_open(_: ?*anyopaque, _: [*:0]const u8, dir: *fsfmt.FsDir) c_int {
    dir.is_open = true;
    return 0;
}
export fn ra8_fs_dir_next(_: *fsfmt.FsDir, out: *fsfmt.FsDirent, present: *bool) c_int {
    present.* = dir_entries > 0;
    if (dir_entries == 0) return 0;
    dir_entries -= 1;
    out.name[0] = 'a';
    out.attr = 0x20;
    out.size_bytes = 7;
    return 0;
}
export fn ra8_fs_dir_close(dir: *fsfmt.FsDir) c_int {
    dir.is_open = false;
    dir_closes += 1;
    return 0;
}
export fn ra8_fs_unlink(_: ?*anyopaque, _: [*:0]const u8) c_int {
    return not_supported;
}
export fn ra8_fs_rename(_: ?*anyopaque, _: [*:0]const u8, _: [*:0]const u8) c_int {
    return not_supported;
}
export fn ra8_fs_mkdir(_: ?*anyopaque, _: [*:0]const u8) c_int {
    return not_supported;
}
export fn ra8_fs_rmdir(_: ?*anyopaque, _: [*:0]const u8) c_int {
    return not_supported;
}
export fn ra8_fs_free_space(_: ?*anyopaque, _: *anyopaque) c_int {
    return not_supported;
}
