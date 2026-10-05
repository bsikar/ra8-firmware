//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VFAT long-name layout helpers shared by the reader and the writer: the
//! 8.3 checksum that ties a chain to its short entry, laying one LFN slot
//! out, and reassembling a chain while a directory is scanned.
//!
//! Each LFN entry carries 13 UTF-16LE units at fixed offsets
//! (1/3/5/7/9, 14/16/18/20/22/24, 28/30). Reader and writer index the one
//! table below, so they cannot disagree about where a character lives.
//! The directory search that uses the reader (`priv_dir_find_long`) is
//! still in `ra8_fs_fat_lfn.c`.
//!
//! Bounded loops: every loop runs at most 13, 11 or `k_lfn_write_max` times.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const LfnState = c.lfn_state_t;

pub const chars_per_ent: u32 = c.k_lfn_chars_per_ent;
pub const write_max: u32 = c.k_lfn_write_max;
pub const max_entries: u32 = c.k_lfn_max_entries;
pub const seq_last: u8 = c.k_lfn_seq_last;
pub const unicode_pad: u16 = c.k_lfn_unicode_pad;
const seq_order_mask: u8 = c.k_lfn_seq_order_mask;
const off_seq: usize = c.k_lfn_off_seq;
const off_checksum: usize = c.k_lfn_off_checksum;
const off_attr: usize = c.k_dir_off_attr;
const name_len: usize = c.k_dir_name_field_len;
const dirent_len: usize = c.k_ra8_fs_dir_entry_bytes;
const attr_lfn: u8 = c.k_ra8_fs_attr_lfn;

/// Byte offset of each of the 13 UTF-16LE units inside an LFN entry.
pub const char_off = [13]u8{ 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

comptime {
    std.debug.assert(char_off.len == chars_per_ent);
}

pub export fn priv_sfn_checksum(name83: [*]const u8) callconv(.C) u8 {
    var sum: u8 = 0;
    for (name83[0..name_len]) |ch| sum = ((sum & 1) << 7) +% (sum >> 1) +% ch;
    return sum;
}

/// Lay out slot `order` (1-based) of the chain for `name[0..nlen]`: the
/// name's units, one NUL right after the last one, 0xFFFF padding past it.
pub export fn priv_lfn_fill_slot(
    ent: [*]u8,
    name: [*c]const u16,
    nlen: u32,
    order: u32,
    is_last: u8,
    csum: u8,
) callconv(.C) void {
    @memset(ent[0..dirent_len], 0);
    ent[off_seq] = @truncate(order | if (is_last != 0) @as(u32, seq_last) else 0);
    ent[off_attr] = attr_lfn;
    ent[off_checksum] = csum;
    const base = (order - 1) * chars_per_ent;
    for (char_off, 0..) |off, i| {
        const pos = base + @as(u32, @intCast(i));
        const val: u16 = if (pos < nlen) name[pos] else if (pos == nlen) 0 else unicode_pad;
        std.mem.writeInt(u16, ent[off..][0..2], val, .little);
    }
}

pub export fn priv_lfn_reset(s: *LfnState) callconv(.C) void {
    @memset(&s.units, 0);
    s.checksum = 0;
    s.have = 0;
}

/// Fold one LFN entry into the chain. An order outside 1..k_lfn_max_entries
/// means a corrupt chain, so that entry is ignored.
pub export fn priv_lfn_add(s: *LfnState, ent: [*]const u8) callconv(.C) void {
    const order: u32 = ent[off_seq] & seq_order_mask;
    if (order < 1 or order > max_entries) return;
    s.checksum = ent[off_checksum];
    s.have = 1;
    const base = (order - 1) * chars_per_ent;
    for (char_off, 0..) |off, i| {
        const pos = base + @as(u32, @intCast(i));
        const val = std.mem.readInt(u16, ent[off..][0..2], .little);
        if (val == 0 or val == unicode_pad) {
            s.units[pos] = 0; // terminator or padding ends this group
            break;
        }
        s.units[pos] = val;
    }
}

/// The reassembled name for the 8.3 entry `name83`, or null when no chain
/// was collected or its checksum does not bind it to this entry.
pub export fn priv_lfn_units_for(s: *const LfnState, name83: [*]const u8, out_units: *u32) callconv(.C) ?[*]const u16 {
    out_units.* = 0;
    if (s.have == 0 or s.units[0] == 0) return null;
    if (s.checksum != priv_sfn_checksum(name83)) return null;
    out_units.* = @intCast(std.mem.indexOfScalar(u16, &s.units, 0) orelse s.units.len);
    return &s.units;
}
