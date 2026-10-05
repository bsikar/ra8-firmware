//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT volume-label read/write: the exFAT half of `ra8_fs_{get,set}_label`.
//!
//! exFAT keeps the label in one root-directory entry, the Volume Label entry:
//! type 0x83 when a label is present, 0x03 (in-use bit clear) when it is not
//! (Microsoft exFAT spec rev 1.00, sec 7.7). It has no secondary entries and
//! no SetChecksum, so reading and rewriting it is a single-entry operation.
//! The FAT half and the lock-bracketed public entry points stay in
//! `ra8_fs_fat_label.c`; the directory walkers stay in the C library and are
//! reached through the extern declarations below.
//!
//! Bounded loops: the root scan stops at `scan_limit` entries; the decode and
//! encode loops at `label_max` (11) code units. No allocation: one 32-byte
//! entry buffer on the stack.

pub const ok: u16 = 0;
pub const err_not_found: u16 = 0x106;

pub const entry_bytes: u32 = 32;
pub const entry_eod: u8 = 0x00;
pub const entry_label: u8 = 0x83;
pub const inuse_bit: u8 = 0x80;
pub const scan_limit: u32 = 65536;
pub const label_max: u32 = 11;
pub const lbl_cnt: u32 = 1;
pub const lbl_name: u32 = 2;

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Dir = c.exfat_dir_t;
pub const Cursor = c.exfat_cursor_t;
pub const SetPos = c.exfat_setpos_t;
pub const Mount = c.ra8_fs_mount_t;

const Located = struct {
    pos: SetPos = .{ .cluster = 0, .index = 0 },
    entry: [entry_bytes]u8 = [_]u8{0} ** entry_bytes,
    present: bool = false,
};

/// Find the Volume Label entry (present or cleared) in the root directory, or
/// the end-of-directory slot a new one would go in. Reads only.
fn locateLabel(m: *const Mount, out: *Located) u16 {
    const label_type: u8 = entry_label & ~inuse_bit;
    var root = std.mem.zeroes(Dir);
    c.priv_exfat_dir_root(m, &root);
    var cur = std.mem.zeroes(Cursor);
    c.priv_exfat_cursor_init(&root, &cur);
    while (cur.scanned < scan_limit) {
        const at: SetPos = .{ .cluster = cur.cluster, .index = cur.entry_in_cluster };
        var e = [_]u8{0} ** entry_bytes;
        const err = c.priv_exfat_next_entry(m, &cur, &e);
        if (err != ok) return err;
        if (e[0] == entry_eod) {
            out.* = .{ .pos = at, .present = false };
            return ok;
        }
        if ((e[0] & ~inuse_bit) == label_type) {
            out.* = .{ .pos = at, .entry = e, .present = true };
            return ok;
        }
    }
    return err_not_found;
}

/// Decode a label entry's UTF-16LE name into NUL-terminated ASCII (low byte
/// of each code unit), truncated to the entry's cap and to `out_len`.
pub fn decodeLabel(entry: *const [entry_bytes]u8, out: [*]u8, out_len: u32) void {
    const n: u32 = @min(@as(u32, entry[lbl_cnt]), label_max);
    var w: u32 = 0;
    var i: u32 = 0;
    while (i < n and w + 1 < out_len) : (i += 1) {
        out[w] = entry[lbl_name + i * 2];
        w += 1;
    }
    out[w] = 0;
}

/// Build an in-use label entry from a NUL-terminated ASCII label (null means
/// empty), capped at `label_max` characters.
pub fn encodeLabel(label: ?[*:0]const u8) [entry_bytes]u8 {
    var entry = [_]u8{0} ** entry_bytes;
    entry[0] = entry_label;
    var n: u32 = 0;
    if (label) |s| {
        while (n < label_max and s[n] != 0) : (n += 1) {
            entry[lbl_name + n * 2] = s[n];
        }
    }
    entry[lbl_cnt] = @intCast(n);
    return entry;
}

pub export fn priv_exfat_get_label(m: *const Mount, out: [*]u8, out_len: u32) callconv(.C) u16 {
    var found: Located = .{};
    const err = locateLabel(m, &found);
    if (err != ok) return err;
    if (!found.present or found.entry[0] != entry_label) {
        out[0] = 0; // no entry, or a cleared (0x03) one: unlabelled
        return ok;
    }
    decodeLabel(&found.entry, out, out_len);
    return ok;
}

pub export fn priv_exfat_set_label(m: *const Mount, label: ?[*:0]const u8) callconv(.C) u16 {
    var found: Located = .{};
    const err = locateLabel(m, &found);
    if (err != ok) return err;
    const entry = encodeLabel(label);
    return c.priv_exfat_write_dir_set(m, found.pos.cluster, found.pos.index, &entry, entry_bytes);
}
