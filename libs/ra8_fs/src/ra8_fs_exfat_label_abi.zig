//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The exFAT volume label (RA8FW-730), ported from ra8_fs_fat_exfat_label.c.
//! priv_exfat_get_label and priv_exfat_set_label scan the root directory for
//! the Volume Label entry (0x83 in use, 0x03 cleared) through the C directory
//! cursor, and rewrite it in place with priv_exfat_write_dir_set. The label is
//! stored as up to 11 UTF-16LE units; only the low byte of each is kept.

pub const ok: c_int = 0;
pub const err_not_found: c_int = 0x106;

/// ra8_fs_fat_types_internal.h constants this unit uses.
pub const entry_bytes: u32 = 32;
pub const entry_eod: u8 = 0x00;
pub const entry_label: u8 = 0x83;
pub const inuse_bit: u8 = 0x80;
pub const scan_limit: u32 = 65536;
pub const lbl_cnt: usize = 1;
pub const lbl_name: usize = 2;
pub const label_max: u32 = 11;

/// Opaque ra8_fs_mount_t; only ever passed through to the C helpers.
pub const Mount = opaque {};

/// Mirror of exfat_dir_t.
pub const Dir = extern struct {
    cluster: u32,
    contig_end: u32,
    self_cluster: u32,
    self_index: u32,
};

/// Mirror of exfat_cursor_t.
pub const Cursor = extern struct {
    cluster: u32,
    entry_in_cluster: u32,
    scanned: u32,
    contig_end: u32,
};

/// Mirror of exfat_setpos_t.
pub const SetPos = extern struct {
    cluster: u32,
    index: u32,
};

extern fn priv_exfat_dir_root(m: ?*const Mount, out: *Dir) void;
extern fn priv_exfat_cursor_init(dir: *const Dir, out: *Cursor) void;
extern fn priv_exfat_next_entry(m: ?*const Mount, cur: *Cursor, out: [*]u8) c_int;
extern fn priv_exfat_write_dir_set(m: ?*const Mount, cluster: u32, idx: u32, set: [*]const u8, bytes: u32) c_int;

const Entry = [entry_bytes]u8;

const Found = struct {
    pos: SetPos = .{ .cluster = 0, .index = 0 },
    entry: Entry = [_]u8{0} ** entry_bytes,
    present: bool = false,
};

/// Scan the root for the label entry, or the end-of-directory slot after it.
fn locate(m: ?*const Mount, found: *Found) c_int {
    const label_type = entry_label & ~inuse_bit;
    var root = Dir{ .cluster = 0, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
    priv_exfat_dir_root(m, &root);
    var cur = Cursor{ .cluster = 0, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
    priv_exfat_cursor_init(&root, &cur);
    while (cur.scanned < scan_limit) {
        const at = SetPos{ .cluster = cur.cluster, .index = cur.entry_in_cluster };
        var e: Entry = [_]u8{0} ** entry_bytes;
        const err = priv_exfat_next_entry(m, &cur, &e);
        if (err != ok) return err;
        if (e[0] == entry_eod) {
            found.pos = at;
            found.present = false;
            return ok;
        }
        if ((e[0] & ~inuse_bit) == label_type) {
            found.pos = at;
            found.entry = e;
            found.present = true;
            return ok;
        }
    }
    return err_not_found;
}

/// Low byte of each stored unit, capped at label_max and out_len - 1.
pub fn decode(entry: *const Entry, out: [*]u8, out_len: u32) void {
    const n = @min(@as(u32, entry[lbl_cnt]), label_max);
    var w: u32 = 0;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        if (w + 1 >= out_len) break;
        out[w] = entry[lbl_name + i * 2];
        w += 1;
    }
    out[w] = 0;
}

/// A fresh in-use label entry for `label` (null or empty clears it).
pub fn encode(label: ?[*:0]const u8) Entry {
    var entry: Entry = [_]u8{0} ** entry_bytes;
    entry[0] = entry_label;
    var n: u32 = 0;
    if (label) |text| {
        while (n < label_max and text[n] != 0) : (n += 1) {
            entry[lbl_name + n * 2] = text[n];
        }
    }
    entry[lbl_cnt] = @intCast(n);
    return entry;
}

pub export fn priv_exfat_get_label(m: ?*const Mount, out: [*]u8, out_len: u32) c_int {
    var found = Found{};
    const err = locate(m, &found);
    if (err != ok) return err;
    if (!found.present or found.entry[0] != entry_label) {
        out[0] = 0; // no entry, or a cleared (0x03) one: unlabelled
        return ok;
    }
    decode(&found.entry, out, out_len);
    return ok;
}

pub export fn priv_exfat_set_label(m: ?*const Mount, label: ?[*:0]const u8) c_int {
    var found = Found{};
    const err = locate(m, &found);
    if (err != ok) return err;
    const entry = encode(label);
    return priv_exfat_write_dir_set(m, found.pos.cluster, found.pos.index, &entry, entry_bytes);
}
