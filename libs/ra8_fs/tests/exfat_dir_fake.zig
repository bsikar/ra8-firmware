//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! In-memory stand-ins for the C exFAT directory walkers the Zig label unit
//! calls. Every test root that imports `ra8_fs` references this file so the
//! test binary links without the C library.

const std = @import("std");
const lbl = @import("ra8_fs").exfat_label;

pub const slots = 8;
pub var dir: [slots][lbl.entry_bytes]u8 = undefined;
pub var read_error: u16 = lbl.ok;
pub var written_index: ?u32 = null;
pub var written_cluster: u32 = 0;
pub var mount_byte: u8 = 0;

pub fn mount() *const lbl.Mount {
    return @ptrCast(&mount_byte);
}

export fn priv_exfat_dir_root(m: *const lbl.Mount, out: *lbl.Dir) callconv(.C) void {
    _ = m;
    out.* = .{ .cluster = 5 };
}

export fn priv_exfat_cursor_init(d: *const lbl.Dir, out: *lbl.Cursor) callconv(.C) void {
    out.* = .{ .cluster = d.cluster };
}

export fn priv_exfat_next_entry(m: *const lbl.Mount, cur: *lbl.Cursor, out: [*]u8) callconv(.C) u16 {
    _ = m;
    if (read_error != lbl.ok) return read_error;
    const i = cur.entry_in_cluster;
    const src: [lbl.entry_bytes]u8 = if (i < slots) dir[i] else [_]u8{0x85} ** lbl.entry_bytes;
    @memcpy(out[0..lbl.entry_bytes], &src);
    cur.entry_in_cluster += 1;
    cur.scanned += 1;
    return lbl.ok;
}

export fn priv_exfat_write_dir_set(m: *const lbl.Mount, cluster: u32, idx: u32, set: [*]const u8, bytes: u32) callconv(.C) u16 {
    _ = m;
    written_cluster = cluster;
    written_index = idx;
    @memcpy(dir[idx][0..bytes], set[0..bytes]);
    return lbl.ok;
}
