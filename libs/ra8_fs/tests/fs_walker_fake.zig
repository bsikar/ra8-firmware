//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! In-memory stand-ins for the C directory walkers the Zig units call. Every
//! test root that imports `ra8_fs` references this file so the test binary
//! links without the C library. The exFAT root is one cluster (5) of `slots`
//! entries; the FAT side is one 512-byte sector.

const std = @import("std");
const c = @import("ra8_fs").c;
const entry_bytes = 32;

pub const slots = 8;
pub var dir: [slots][entry_bytes]u8 = undefined;
pub var read_error: u16 = 0;
pub var written_index: ?u32 = null;
pub var written_cluster: u32 = 0;
pub var mount_store: c.ra8_fs_mount_t = std.mem.zeroes(c.ra8_fs_mount_t);

pub var sector: [512]u8 = undefined;
pub var have83: u8 = 1;
pub var find83_err: u16 = 0;
pub var find_long_err: u16 = 0;
pub var resolve_err: u16 = 0;
pub var sector_read_err: u16 = 0;
pub var sector_written: ?u64 = null;
pub var find_set_err: u16 = 0;
pub var set_count: u32 = 2;
pub var checksum_bytes: u32 = 0;

pub fn mount() *const c.ra8_fs_mount_t {
    return &mount_store;
}

pub fn reset(fs_type: u8) void {
    for (&dir) |*e| e.* = [_]u8{0} ** entry_bytes;
    dir[0][0] = 0x81; // allocation bitmap
    dir[1][0] = 0x82; // up-case table
    mount_store = std.mem.zeroes(c.ra8_fs_mount_t);
    mount_store.in_use = 1;
    mount_store.type = fs_type;
    sector = [_]u8{0} ** 512;
    read_error = 0;
    written_index = null;
    have83 = 1;
    find83_err = 0;
    find_long_err = 0;
    resolve_err = 0;
    sector_read_err = 0;
    sector_written = null;
    find_set_err = 0;
    set_count = 2;
    checksum_bytes = 0;
}

export fn priv_exfat_dir_root(m: [*c]const c.ra8_fs_mount_t, out: [*c]c.exfat_dir_t) callconv(.C) void {
    _ = m;
    out.* = std.mem.zeroes(c.exfat_dir_t);
    out.*.cluster = 5;
}

export fn priv_exfat_cursor_init(d: [*c]const c.exfat_dir_t, out: [*c]c.exfat_cursor_t) callconv(.C) void {
    out.* = std.mem.zeroes(c.exfat_cursor_t);
    out.*.cluster = d.*.cluster;
}

export fn priv_exfat_next_entry(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]u8) callconv(.C) u16 {
    _ = m;
    if (read_error != 0) return read_error;
    const i = cur.*.entry_in_cluster;
    const src: [entry_bytes]u8 = if (i < slots) dir[i] else [_]u8{0x85} ** entry_bytes;
    @memcpy(out[0..entry_bytes], &src);
    cur.*.entry_in_cluster += 1;
    cur.*.scanned += 1;
    return 0;
}

export fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.C) u16 {
    _ = m;
    written_cluster = cluster;
    written_index = idx;
    @memcpy(dir[idx][0..bytes], set[0..bytes]);
    return 0;
}

export fn priv_exfat_find_set(m: [*c]const c.ra8_fs_mount_t, d: [*c]const c.exfat_dir_t, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, max_pos: u32, out_count: [*c]u32, file_copy: [*c]u8, strm_copy: [*c]u8) callconv(.C) u16 {
    _ = .{ m, path, file_copy, strm_copy };
    if (find_set_err != 0) return find_set_err;
    const n = @min(set_count, max_pos);
    for (0..n) |k| pos[k] = .{ .cluster = d.*.cluster, .index = @intCast(2 + k) };
    out_count.* = n;
    return 0;
}

export fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.C) u16 {
    checksum_bytes = bytes;
    return 0xA000 | @as(u16, set[4]);
}

export fn priv_wr16(p: [*c]u8, v: u16) callconv(.C) void {
    std.mem.writeInt(u16, p[0..2], v, .little);
}

export fn priv_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.dir_loc_t, out_leaf: [*c][*c]const u8) callconv(.C) u16 {
    _ = m;
    if (resolve_err != 0) return resolve_err;
    out_parent.* = std.mem.zeroes(c.dir_loc_t);
    out_parent.*.is_root = 1;
    out_leaf.* = path;
    return 0;
}

export fn priv_path_to_83(path: [*c]const u8, out11: [*c]u8) callconv(.C) u8 {
    _ = .{ path, out11 };
    return have83;
}

export fn priv_dir_find(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, name83: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.C) u16 {
    _ = .{ m, loc, name83, out_entry };
    if (find83_err != 0) return find83_err;
    out_lba.* = 7;
    out_off.* = 64;
    return 0;
}

export fn priv_dir_find_long(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, want: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.C) u16 {
    _ = .{ m, loc, want, out_entry };
    if (find_long_err != 0) return find_long_err;
    out_lba.* = 9;
    out_off.* = 96;
    return 0;
}

export fn priv_sec_walk() callconv(.C) [*c]u8 {
    return &sector;
}

export fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.C) u16 {
    _ = .{ m, lba, buf };
    return sector_read_err;
}

export fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.C) u16 {
    _ = .{ m, buf };
    sector_written = lba;
    return 0;
}

export fn priv_fat_entry_apply_attr(entry: [*c]u8, set_mask: u8, clear_mask: u8) callconv(.C) void {
    entry[11] = (entry[11] & ~clear_mask) | set_mask;
}
