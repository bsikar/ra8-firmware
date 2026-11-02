//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Runtime volume-label read/set: the FAT half of `ra8_fs_get_label()` /
//! `ra8_fs_set_label()`, plus the lock-bracketed public entry points that
//! dispatch to the exFAT half in `ra8_fs_exfat_label_abi.zig`.
//!
//! On FAT the label lives in two places kept in step: the boot sector's
//! `BS_VolLab` field, and a root-directory `ATTR_VOLUME_ID` entry (the copy a
//! desktop shows and edits). Setting a real label writes both -- creating the
//! root entry when absent -- so `fsck.fat` never sees a blank or a mismatched
//! label; clearing the label restores the `"NO NAME    "` sentinel in the boot
//! sector and removes the root entry. Reading prefers the root entry, falling
//! back to `BS_VolLab`, and reports the unlabelled sentinel as the empty
//! string.
//!
//! The directory walkers, the sector backend, and the label-field writer stay
//! in the C library and are reached through the extern declarations below.
//! Bounded loops: the root walks terminate on end-of-directory or the
//! walker's own cluster-cycle guard; the decode and copy loops are bounded by
//! the 11-byte label field. No allocation: the walker's own sector buffer.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_no_mem: u16 = c.k_ra8_err_no_mem;

const label_len: u32 = @intCast(c.k_fmt_label_len);
const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const off_name: usize = @intCast(c.k_dir_off_name);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const marker_free_perm: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const attr_volume_id: u8 = @intCast(c.k_ra8_fs_attr_volume_id);
const type_fat32 = c.k_ra8_fs_type_fat32;
const type_exfat = c.k_ra8_fs_type_exfat;

const no_name = "NO NAME    ";

extern fn priv_read_sector(m: *const Mount, lba: u64, buf: [*]u8) u16;
extern fn priv_write_sector(m: *const Mount, lba: u64, buf: [*]const u8) u16;
extern fn priv_sec_walk() [*]u8;
extern fn priv_dir_eps(m: *const Mount) u32;
extern fn priv_dir_walk_init_loc(m: *const Mount, loc: *const c.dir_loc_t, w: *c.dir_walk_t) void;
extern fn priv_dir_walk_next_sector(m: *const Mount, w: *c.dir_walk_t, out_eod: *u8) u16;
extern fn priv_fmt_label_field(dst: [*]u8, label: ?[*:0]const u8) void;
extern fn priv_strlen(s: [*:0]const u8) u32;
extern fn priv_fat_entry_stamp_create(entry: [*]u8) void;
extern fn priv_fat_entry_stamp_write(entry: [*]u8) void;
extern fn priv_exfat_get_label(m: *const Mount, out: [*]u8, out_len: u32) u16;
extern fn priv_exfat_set_label(m: *const Mount, label: ?[*:0]const u8) u16;
extern fn priv_lock_acquire() void;
extern fn priv_lock_release() void;

/// Byte offset of `BS_VolLab` in this volume's boot sector: FAT32 carries its
/// extended boot signature (and so its label) at a different offset than
/// FAT12/16, because the FAT32 BPB is longer.
fn bootLabelOff(m: *const Mount) u32 {
    return if (m.type == type_fat32)
        @as(u32, @intCast(c.k_fmt_off_f32_label))
    else
        @as(u32, @intCast(c.k_fmt_off_f16_label));
}

/// Decode an 11-byte packed FAT label into a trimmed string: strips trailing
/// spaces (the only padding the FAT spec allows) and reports the `"NO NAME    "`
/// sentinel as the empty string. The result is NUL-terminated and truncated to
/// `out_len` bytes.
fn labelFromRaw(raw11: *const [label_len]u8, out: [*]u8, out_len: usize) void {
    if (std.mem.eql(u8, raw11, no_name)) {
        out[0] = 0;
        return;
    }
    var n: usize = label_len;
    while (n > 0 and raw11[n - 1] == ' ') n -= 1;
    var w: usize = 0;
    for (raw11[0..n]) |ch| {
        if (w + 1 >= out_len) break;
        out[w] = ch;
        w += 1;
    }
    out[w] = 0;
}

/// Find the root directory's `ATTR_VOLUME_ID` entry: an in-use entry whose
/// attribute byte carries the volume-id bit but is not the long-name marker.
/// Walks the root (a FAT12/16 fixed region or a FAT32 root cluster chain)
/// until end-of-directory. Reads only.
fn findVolId(m: *const Mount, out_lba: *u64, out_off: *u32, out_entry: *[entry_bytes]u8) u16 {
    const loc = std.mem.zeroes(c.dir_loc_t);
    var w: c.dir_walk_t = undefined;
    priv_dir_walk_init_loc(m, &loc, &w);
    var eod: u8 = 0;
    const buf = priv_sec_walk();
    while (eod == 0) {
        var err = priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) return err;
        const eps = priv_dir_eps(m);
        var e: u32 = 0;
        while (e < eps) : (e += 1) {
            const off: usize = @as(usize, e) * entry_bytes;
            const name0 = buf[off + off_name];
            const attr = buf[off + off_attr];
            if (name0 == marker_free_perm) return err_not_found;
            if (name0 == marker_free_used) continue;
            if (attr == attr_lfn) continue;
            if ((attr & attr_volume_id) != 0) {
                out_lba.* = w.cur_lba;
                out_off.* = @intCast(off);
                @memcpy(out_entry, buf[off..][0..entry_bytes]);
                return ok;
            }
        }
        err = priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) return err;
    }
    return err_not_found;
}

/// Find the first free slot in the root directory: a never-used (0x00) or
/// deleted (0xE5) entry, where a fresh volume-label entry can be written.
fn findFreeRoot(m: *const Mount, out_lba: *u64, out_off: *u32) u16 {
    const loc = std.mem.zeroes(c.dir_loc_t);
    var w: c.dir_walk_t = undefined;
    priv_dir_walk_init_loc(m, &loc, &w);
    var eod: u8 = 0;
    const buf = priv_sec_walk();
    while (eod == 0) {
        var err = priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) return err;
        const eps = priv_dir_eps(m);
        var e: u32 = 0;
        while (e < eps) : (e += 1) {
            const off: usize = @as(usize, e) * entry_bytes;
            const name0 = buf[off + off_name];
            if (name0 == marker_free_perm or name0 == marker_free_used) {
                out_lba.* = w.cur_lba;
                out_off.* = @intCast(off);
                return ok;
            }
        }
        err = priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) return err;
    }
    return err_no_mem;
}

/// Write the boot sector's `BS_VolLab` field: a read-modify-write of sector 0
/// through `priv_fmt_label_field`, which writes the padded label (or the
/// `"NO NAME    "` sentinel when `label` is null or empty) and nothing else.
fn bootSetLabel(m: *const Mount, label: ?[*:0]const u8) u16 {
    const boot = priv_sec_walk();
    const err = priv_read_sector(m, 0, boot);
    if (err != ok) return err;
    priv_fmt_label_field(boot + bootLabelOff(m), label);
    return priv_write_sector(m, 0, boot);
}

/// Mark the root entry at `lba`/`off` deleted: the 0xE5 marker on the first
/// name byte, every other byte of the sector preserved.
fn delEntry(m: *const Mount, lba: u64, off: u32) u16 {
    const sec = priv_sec_walk();
    const err = priv_read_sector(m, lba, sec);
    if (err != ok) return err;
    sec[off + off_name] = marker_free_used;
    return priv_write_sector(m, lba, sec);
}

/// Write a root-directory `ATTR_VOLUME_ID` entry at `lba`/`off`: a fresh slot
/// is zeroed first and stamped with a create time; an existing entry keeps its
/// create fields and gets a write-time stamp. Either way the 11-byte name
/// becomes the padded label and the attribute byte becomes ATTR_VOLUME_ID.
fn putVolId(m: *const Mount, lba: u64, off: u32, label: ?[*:0]const u8, fresh: bool) u16 {
    const sec = priv_sec_walk();
    const err = priv_read_sector(m, lba, sec);
    if (err != ok) return err;
    const at: usize = off;
    if (fresh) @memset(sec[at..][0..entry_bytes], 0);
    priv_fmt_label_field(sec + at + off_name, label);
    sec[at + off_attr] = attr_volume_id;
    if (fresh) {
        priv_fat_entry_stamp_create(sec + at);
    } else {
        priv_fat_entry_stamp_write(sec + at);
    }
    return priv_write_sector(m, lba, sec);
}

/// Read a FAT volume's label: the root `ATTR_VOLUME_ID` entry (the copy a
/// desktop shows) when it exists, else the boot sector's `BS_VolLab`.
fn getLabelFat(m: *const Mount, out: [*]u8, out_len: u32) u16 {
    var raw: [label_len]u8 = @splat(0);
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    const ferr = findVolId(m, &lba, &off, &entry);
    if (ferr == ok) {
        @memcpy(&raw, entry[off_name..][0..label_len]);
    } else if (ferr == err_not_found) {
        const boot = priv_sec_walk();
        const berr = priv_read_sector(m, 0, boot);
        if (berr != ok) return berr;
        @memcpy(&raw, (boot + bootLabelOff(m))[0..label_len]);
    } else {
        return ferr;
    }
    labelFromRaw(&raw, out, out_len);
    return ok;
}

/// Set a FAT volume's label: `BS_VolLab` (the sentinel when clearing), then
/// the root-directory entry reconciled against it -- a real label rewrites the
/// existing entry or creates one in a free slot; clearing deletes it. Keeping
/// the two copies in step is what keeps `fsck.fat` quiet.
fn setLabelFat(m: *const Mount, label: ?[*:0]const u8) u16 {
    const clearing = label == null or label.?[0] == 0;
    var err = bootSetLabel(m, label);
    if (err != ok) return err;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    const ferr = findVolId(m, &lba, &off, &entry);
    if (ferr != ok and ferr != err_not_found) return ferr;
    if (clearing) return if (ferr == ok) delEntry(m, lba, off) else ok;
    if (ferr == ok) return putVolId(m, lba, off, label, false);
    err = findFreeRoot(m, &lba, &off);
    if (err != ok) return err;
    return putVolId(m, lba, off, label, true);
}

/// Read the volume label -- the guarded body of `ra8_fs_get_label()`. The
/// public entry point brackets this with the library lock.
fn getLabelLocked(handle: *const Mount, out: ?[*]u8, out_len: u32) u16 {
    if (out == null) return err_null_ptr;
    if (out_len == 0) return err_invalid_arg;
    if (handle.in_use == 0) return err_invalid_state;
    out.?[0] = 0;
    if (handle.type == type_exfat) return priv_exfat_get_label(handle, out.?, out_len);
    return getLabelFat(handle, out.?, out_len);
}

/// Set the volume label -- the guarded body of `ra8_fs_set_label()`,
/// rejecting a label longer than the 11-byte field.
fn setLabelLocked(handle: *const Mount, label: ?[*:0]const u8) u16 {
    if (handle.in_use == 0) return err_invalid_state;
    if (label != null and priv_strlen(label.?) > label_len) return err_invalid_arg;
    if (handle.type == type_exfat) return priv_exfat_set_label(handle, label);
    return setLabelFat(handle, label);
}

pub export fn ra8_fs_get_label(handle: ?*const Mount, out: ?[*]u8, out_len: u32) callconv(.c) u16 {
    priv_lock_acquire();
    defer priv_lock_release();
    if (handle == null or out == null) return err_null_ptr;
    return getLabelLocked(handle.?, out.?, out_len);
}

pub export fn ra8_fs_set_label(handle: ?*const Mount, label: ?[*:0]const u8) callconv(.c) u16 {
    priv_lock_acquire();
    defer priv_lock_release();
    if (handle == null) return err_null_ptr;
    return setLabelLocked(handle.?, label);
}
