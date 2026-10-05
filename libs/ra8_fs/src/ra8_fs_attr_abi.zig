//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_fs_set_attr`: set or clear the host-controlled attribute bits
//! (read-only, hidden, system, archive) on a file or directory.
//!
//! FAT patches `DIR_Attr` in the short entry, found by 8.3 name first and by
//! long name second. exFAT patches FileAttributes in the File entry, then
//! recomputes the SetChecksum over the whole entry set and rewrites the File
//! entry. The directory walkers stay in the C library; this unit reaches
//! them, and every C type it touches, through `fs_c.zig`.

const std = @import("std");
const c = @import("fs_c.zig").c;
const lock = @import("ra8_fs_lock_abi.zig");

pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
pub const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
pub const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
pub const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
pub const err_not_found: u16 = c.k_ra8_err_not_found;

pub const settable: u8 = c.k_ra8_fs_attr_settable;
const name83_len: usize = @intCast(c.k_max_8_3_name);
const dirent_len: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const entry_len: usize = @intCast(c.k_exfat_entry_bytes);
pub const set_max: usize = @intCast(c.k_exfat_set_max_entries);
pub const off_attr: usize = @intCast(c.k_exfat_off_file_attr);
pub const off_csum: usize = @intCast(c.k_exfat_off_file_csum);

fn setAttrFat(m: *const Mount, path: [*:0]const u8, set_mask: u8, clear_mask: u8) u16 {
    var parent = std.mem.zeroes(c.dir_loc_t);
    var leaf: [*c]const u8 = null;
    const rerr = c.priv_resolve_parent(m, path, &parent, &leaf);
    if (rerr != ok) return rerr;
    var name83 = [_]u8{0} ** name83_len;
    const have83 = c.priv_path_to_83(leaf, &name83);
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry = [_]u8{0} ** dirent_len;
    var err: u16 = err_not_found;
    if (have83 != 0) err = c.priv_dir_find(m, &parent, &name83, &lba, &off, &entry);
    if (err == err_not_found) err = c.priv_dir_find_long(m, &parent, leaf, &lba, &off, &entry);
    if (err != ok) return err;
    const sec = c.priv_sec_walk();
    err = c.priv_read_sector(m, lba, sec);
    if (err != ok) return err;
    c.priv_fat_entry_apply_attr(sec + off, set_mask, clear_mask);
    return c.priv_write_sector(m, lba, sec);
}

fn setAttrExfat(m: *const Mount, path: [*:0]const u8, set_mask: u8, clear_mask: u8) u16 {
    var root = std.mem.zeroes(c.exfat_dir_t);
    c.priv_exfat_dir_root(m, &root);
    var pos = std.mem.zeroes([set_max]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_e = [_]u8{0} ** entry_len;
    var strm_e = [_]u8{0} ** entry_len;
    var err = c.priv_exfat_find_set(m, &root, path, &pos, set_max, &count, &file_e, &strm_e);
    if (err != ok) return err;
    var eset = [_]u8{0} ** (set_max * entry_len);
    const n: usize = @min(count, set_max);
    for (pos[0..n], 0..) |p, k| {
        var one: c.exfat_cursor_t = .{
            .cluster = p.cluster,
            .entry_in_cluster = p.index,
            .scanned = 0,
            .contig_end = 0,
        };
        err = c.priv_exfat_next_entry(m, &one, &eset[k * entry_len]);
        if (err != ok) return err;
    }
    eset[off_attr] = (eset[off_attr] & ~clear_mask) | set_mask;
    const bytes: u32 = @intCast(n * entry_len);
    c.priv_wr16(&eset[off_csum], c.priv_exfat_set_checksum(&eset, bytes));
    return c.priv_exfat_write_dir_set(m, pos[0].cluster, pos[0].index, &eset, entry_len);
}

fn setAttrLocked(handle: ?*const Mount, path: ?[*:0]const u8, set_mask: u8, clear_mask: u8) u16 {
    const m = handle orelse return err_null_ptr;
    const p = path orelse return err_null_ptr;
    if (m.in_use == 0) return err_invalid_state;
    // Only the four host-controlled bits, and never the same bit both ways.
    if ((set_mask & ~settable) != 0 or (clear_mask & ~settable) != 0 or (set_mask & clear_mask) != 0) {
        return err_invalid_arg;
    }
    var i: usize = 0;
    while (p[i] == '/') i += 1;
    if (p[i] == 0) return err_invalid_arg; // the volume root has no entry to patch
    if (m.type == c.k_ra8_fs_type_exfat) return setAttrExfat(m, p, set_mask, clear_mask);
    return setAttrFat(m, p, set_mask, clear_mask);
}

pub export fn ra8_fs_set_attr(handle: ?*const Mount, path: ?[*:0]const u8, set_mask: u8, clear_mask: u8) callconv(.C) u16 {
    lock.priv_lock_acquire();
    defer lock.priv_lock_release();
    return setAttrLocked(handle, path, set_mask, clear_mask);
}
