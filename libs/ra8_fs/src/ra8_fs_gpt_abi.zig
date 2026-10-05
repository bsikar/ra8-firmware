//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GPT partition locators for the mount path: find the volume to mount on a
//! GPT disk (`priv_gpt_locate_volume`) or a chosen entry
//! (`priv_gpt_locate_partition`).
//!
//! The header at LBA 1 must carry "EFI PART", a non-zero entry-array LBA and
//! the standard 128-byte entry size. The volume scan prefers the first Basic
//! Data entry and falls back to the first used one. Sectors are read into
//! `g_fs_scratch`, so callers re-read their own sector afterwards.
//!
//! Bounded loops: the entry walk stops at `k_gpt_entry_scan_max` (the UEFI
//! minimum array size), whatever the header claims.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
pub const err_not_found: u16 = c.k_ra8_err_not_found;
pub const err_not_supported: u16 = c.k_ra8_err_not_supported;
pub const err_out_of_range: u16 = c.k_ra8_err_out_of_range;
pub const err_validation: u16 = c.k_ra8_err_validation_failed;

pub const header_lba: u64 = c.k_gpt_header_lba;
pub const entry_bytes: u32 = c.k_gpt_entry_bytes;
pub const scan_max: u32 = c.k_gpt_entry_scan_max;
const off_entry_lba: usize = c.k_gpt_off_entry_lba;
const off_entry_count: usize = c.k_gpt_off_entry_count;
const off_entry_size: usize = c.k_gpt_off_entry_size;
const off_first_lba: usize = c.k_gpt_entry_off_first_lba;
const guid_len: usize = c.k_gpt_guid_len;

pub const signature = "EFI PART";

/// Basic Data partition type GUID EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, as
/// stored on disk (mixed-endian).
pub const basic_data_guid = [16]u8{
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
};

const Geom = struct { entry_lba: u64, count: u32 };

fn scratch() *[c.k_ra8_fs_sector_max]u8 {
    return &c.g_fs_scratch;
}

fn rd(comptime T: type, buf: []const u8, off: usize) T {
    return std.mem.readInt(T, buf[off..][0..@sizeOf(T)], .little);
}

/// An entry is used when its type GUID is not all zero.
fn isUsed(entry: []const u8) bool {
    for (entry[0..guid_len]) |b| {
        if (b != 0) return true;
    }
    return false;
}

fn readGeom(m: *const Mount, out: *Geom) u16 {
    const s = scratch();
    const err = c.priv_read_sector(m, header_lba, s);
    if (err != ok) return err;
    if (!std.mem.eql(u8, s[0..signature.len], signature)) return err_validation;
    const entry_lba = rd(u64, s, off_entry_lba);
    if (entry_lba == 0) return err_validation;
    if (rd(u32, s, off_entry_size) != entry_bytes) return err_not_supported;
    out.* = .{ .entry_lba = entry_lba, .count = @min(rd(u32, s, off_entry_count), scan_max) };
    return ok;
}

fn scanEntries(m: *const Mount, g: Geom, out_base: *u64) u16 {
    const eps = c.priv_bps(m) / entry_bytes;
    const s = scratch();
    var basic: u64 = 0;
    var any: u64 = 0;
    var i: u32 = 0;
    while (i < g.count) : (i += 1) {
        const off = (i % eps) * entry_bytes;
        if (off == 0) {
            const err = c.priv_read_sector(m, g.entry_lba + i / eps, s);
            if (err != ok) return err;
        }
        const entry = s[off..][0..entry_bytes];
        if (!isUsed(entry)) continue;
        const first = rd(u64, entry, off_first_lba);
        if (first == 0) continue;
        if (any == 0) any = first;
        if (basic == 0 and std.mem.eql(u8, entry[0..guid_len], &basic_data_guid)) basic = first;
    }
    const pick = if (basic != 0) basic else any;
    if (pick == 0) return err_not_found;
    out_base.* = pick;
    return ok;
}

pub export fn priv_gpt_locate_volume(m: *const Mount, out_base: *u64) callconv(.C) u16 {
    var g: Geom = undefined;
    const err = readGeom(m, &g);
    if (err != ok) return err;
    return scanEntries(m, g, out_base);
}

pub export fn priv_gpt_locate_partition(m: *const Mount, index: u8, out_base: *u64) callconv(.C) u16 {
    var g: Geom = undefined;
    const err = readGeom(m, &g);
    if (err != ok) return err;
    if (index >= g.count) return err_out_of_range;
    const eps = c.priv_bps(m) / entry_bytes;
    const s = scratch();
    const rerr = c.priv_read_sector(m, g.entry_lba + index / eps, s);
    if (rerr != ok) return rerr;
    const entry = s[(index % eps) * entry_bytes ..][0..entry_bytes];
    if (!isUsed(entry)) return err_not_found;
    const base = rd(u64, entry, off_first_lba);
    if (base == 0) return err_validation;
    out_base.* = base;
    return ok;
}
