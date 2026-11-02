//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Path metadata lookup: what is at this path, without opening it (`stat`).
//!
//! Port of `ra8_fs_fat_stat.c`. `stat` resolves a path exactly as `open` does
//! and then stops at the directory entry, reading the attribute byte, the size
//! and the first cluster straight out of it. Not opening anything is the whole
//! point: a directory opens perfectly well and reports `DIR_FileSize` 0, so an
//! open-based `stat` cannot tell a folder from an empty file, and it spends
//! one of the four file-table slots to fail at it.
//!
//! The path resolution, the 8.3 packer, the two directory finders and the
//! exFAT set finder stay in C or in their own ports (`ra8_fs_lfn_find_abi`),
//! reached through `fs_c.zig`; this unit owns the entry-to-stat decode and the
//! lock brackets.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;
pub const Stat = c.ra8_fs_stat_t;
pub const Timestamp = c.ra8_fs_timestamp_t;
pub const DateTime = c.ra8_fs_datetime_t;

pub const ok: u16 = c.k_ra8_ok;
pub const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
pub const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
pub const err_not_found: u16 = c.k_ra8_err_not_found;

// FAT/exFAT packed timestamp field geometry and validation bounds. The C enum
// was file-local, so the port re-derives the same spec numbers.
const fat_year_shift: u5 = 9;
const fat_month_shift: u5 = 5;
const fat_month_mask: u16 = 0x0F;
const fat_day_mask: u16 = 0x1F;
const fat_hour_shift: u5 = 11;
const fat_hour_mask: u16 = 0x1F;
const fat_minute_shift: u5 = 5;
const fat_minute_mask: u16 = 0x3F;
const fat_second_mask: u16 = 0x1F;
const tenths_per_second: u8 = 100;
const month_max: u8 = 12;
const hour_max: u8 = 23;
const minute_max: u8 = 59;
const second_max: u8 = 59;
const creation_tenth_max: u8 = 199;
const packed_time_mask: u32 = 0xFFFF;
const utc_sign_bit: i32 = 0x40;
const utc_field_modulus: i32 = 128;

const dirent_len: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const entry_len: usize = @intCast(c.k_exfat_entry_bytes);
const set_max: usize = @intCast(c.k_exfat_set_max_entries);
const name83_len: usize = @intCast(c.k_max_8_3_name);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_file_size: usize = @intCast(c.k_dir_off_file_size);
const off_crt_date: usize = @intCast(c.k_dir_off_crt_date);
const off_crt_time: usize = @intCast(c.k_dir_off_crt_time);
const off_crt_time_tenth: usize = @intCast(c.k_dir_off_crt_time_tenth);
const off_wrt_date: usize = @intCast(c.k_dir_off_wrt_date);
const off_wrt_time: usize = @intCast(c.k_dir_off_wrt_time);
const off_lst_acc_date: usize = @intCast(c.k_dir_off_lst_acc_date);
const off_file_ctime: usize = @intCast(c.k_exfat_off_file_ctime);
const off_file_mtime: usize = @intCast(c.k_exfat_off_file_mtime);
const off_file_atime: usize = @intCast(c.k_exfat_off_file_atime);
const off_file_c10ms: usize = @intCast(c.k_exfat_off_file_c10ms);
const off_file_m10ms: usize = @intCast(c.k_exfat_off_file_m10ms);
const off_file_cutc: usize = @intCast(c.k_exfat_off_file_cutc);
const off_file_mutc: usize = @intCast(c.k_exfat_off_file_mutc);
const off_file_autc: usize = @intCast(c.k_exfat_off_file_autc);
const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);

const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);

/// Does `path` name the volume root rather than an entry inside it? The root
/// has no directory entry of its own, so it cannot be looked up; it is
/// answered from the mount geometry instead. Any run of leading slashes with
/// nothing after it is the root, which covers both "" and "/".
pub fn path_is_root(path: [*c]const u8) bool {
    var i: usize = 0;
    while (path[i] == '/') i += 1;
    return path[i] == 0;
}

/// Is a decoded FAT civil date/time within every legal calendar bound? There
/// is deliberately no upper-bound check on the day: FAT stores it in five
/// bits, so `date & fat_day_mask` cannot exceed 31, which is already the
/// largest civil day. Only `day == 0` is representable and illegal.
fn stat_fat_time_valid(v: *const DateTime, tenth: u8, have_tenth: bool) bool {
    if (v.month == 0) return false;
    if (v.month > month_max) return false;
    if (v.day == 0) return false;
    if (v.hour > hour_max) return false;
    if (v.minute > minute_max) return false;
    if (v.second > second_max) return false;
    if (have_tenth and tenth > creation_tenth_max) return false;
    return true;
}

/// Decode one FAT-format packed date/time pair into a public timestamp:
/// FAT's even-second field and optional creation centiseconds, then every
/// out-of-range calendar component rejected. FAT carries no zone, so the
/// UTC-offset validity stays false.
pub fn stat_decode_fat(date: u16, time: u16, tenth: u8, have_tenth: bool, out: *Timestamp) void {
    var v: DateTime = std.mem.zeroes(DateTime);
    v.year = @intCast(@as(u32, @intCast(c.k_fs_time_epoch_year)) + (date >> fat_year_shift));
    v.month = @intCast((date >> fat_month_shift) & fat_month_mask);
    v.day = @intCast(date & fat_day_mask);
    v.hour = @intCast((time >> fat_hour_shift) & fat_hour_mask);
    v.minute = @intCast((time >> fat_minute_shift) & fat_minute_mask);
    v.second = @intCast((time & fat_second_mask) * 2);
    if (have_tenth) {
        v.second += @intFromBool(tenth >= tenths_per_second);
        v.centisecond = tenth % tenths_per_second;
    }
    if (!stat_fat_time_valid(&v, tenth, have_tenth)) {
        out.* = std.mem.zeroes(Timestamp);
        return;
    }
    out.value = v;
    out.valid = true;
    out.utc_offset_valid = false;
}

/// Decode one exFAT packed stamp, 10-ms increment, and UTC-offset byte: the
/// FAT-compatible packed civil fields first, then exFAT's signed 15-minute
/// UTC offset only when its validity bit is set. A clear or invalid offset
/// never masquerades as UTC+00:00.
pub fn stat_decode_exfat(packed_stamp: u32, tenth: u8, have_tenth: bool, utc: u8, out: *Timestamp) void {
    const date: u16 = @intCast(packed_stamp >> 16);
    const time: u16 = @intCast(packed_stamp & packed_time_mask);
    stat_decode_fat(date, time, tenth, have_tenth, out);
    if (!out.valid) return;
    if (utc & @as(u8, @intCast(c.k_fs_utc_valid_bit)) == 0) return;
    var steps: i32 = @intCast(utc & @as(u8, @intCast(c.k_fs_utc_field_mask)));
    if (steps & utc_sign_bit != 0) {
        steps -= utc_field_modulus;
    }
    const minutes: i32 = steps * @as(i32, @intCast(c.k_fs_utc_step_min));
    if (minutes < c.k_fs_utc_span_min) return;
    if (minutes > c.k_fs_utc_span_max) return;
    out.value.utc_offset_min = @intCast(minutes);
    out.utc_offset_valid = true;
}

/// Decode all three timestamp fields from one FAT directory entry: creation,
/// modification, and the date-only access field.
fn stat_fat_times(entry: [*]const u8, out: *Stat) void {
    stat_decode_fat(
        c.priv_rd16(entry + off_crt_date),
        c.priv_rd16(entry + off_crt_time),
        entry[off_crt_time_tenth],
        true,
        &out.created,
    );
    stat_decode_fat(
        c.priv_rd16(entry + off_wrt_date),
        c.priv_rd16(entry + off_wrt_time),
        0,
        false,
        &out.modified,
    );
    stat_decode_fat(c.priv_rd16(entry + off_lst_acc_date), 0, 0, false, &out.accessed);
}

/// Decode all three timestamp fields from one exFAT File entry: creation,
/// modification, and access stamps plus their UTC markers.
fn stat_exfat_times(entry: [*]const u8, out: *Stat) void {
    stat_decode_exfat(
        c.priv_rd32(entry + off_file_ctime),
        entry[off_file_c10ms],
        true,
        entry[off_file_cutc],
        &out.created,
    );
    stat_decode_exfat(
        c.priv_rd32(entry + off_file_mtime),
        entry[off_file_m10ms],
        true,
        entry[off_file_mutc],
        &out.modified,
    );
    stat_decode_exfat(c.priv_rd32(entry + off_file_atime), 0, false, entry[off_file_autc], &out.accessed);
}

/// Fill in `out` for the volume root itself: it always exists and is always a
/// directory. Its `first_cluster` is the FAT32 / exFAT root cluster, and 0 on
/// FAT12/16 where the root is a fixed sector region with no chain.
fn stat_root(m: *const Mount, out: *Stat) void {
    out.size_bytes = 0;
    out.first_cluster = m.root_cluster;
    out.attr = attr_directory;
    out.is_directory = true;
}

/// Translate a 32-byte FAT directory entry into a stat result. The attribute
/// byte is copied through verbatim (read-only, hidden and system survive) and
/// `is_directory` comes from the ATTR_DIRECTORY bit. A directory's
/// `DIR_FileSize` is 0 by specification; it is forced to 0 here anyway so a
/// volume written by some other implementation cannot report a directory with
/// a non-zero length.
pub fn entry_to_stat(entry: [*]const u8, out: *Stat) void {
    const attr: u8 = entry[off_attr];
    out.attr = attr;
    out.is_directory = (attr & attr_directory) != 0;
    out.first_cluster = c.priv_entry_first_cluster(entry);
    out.size_bytes = c.priv_rd32(entry + off_file_size);
    stat_fat_times(entry, out);
    if (out.is_directory) {
        out.size_bytes = 0;
    }
}

/// `stat` a name on an exFAT volume, at any depth: resolve the parent, then
/// `priv_exfat_find_set` hands back the whole Stream entry plus the File
/// entry's attribute byte, so a directory is told from a file by the bit that
/// says so and the lengths come out of the same 32 bytes.
fn stat_exfat(m: *const Mount, path: [*c]const u8, out: *Stat) u16 {
    var parent = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    const pe = c.priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (pe != ok) return pe;
    var pos: [set_max]c.exfat_setpos_t = @splat(std.mem.zeroes(c.exfat_setpos_t));
    var count: u32 = 0;
    var file_e: [entry_len]u8 = @splat(0);
    var strm: [entry_len]u8 = @splat(0);
    const err = c.priv_exfat_find_set(
        m,
        &parent,
        leaf,
        &pos,
        @intCast(set_max),
        &count,
        &file_e,
        &strm,
    );
    if (err != ok) return err;
    const sp: [*]const u8 = &strm;
    out.attr = file_e[off_file_attr];
    out.is_directory = (out.attr & attr_directory) != 0;
    out.first_cluster = c.priv_rd32(sp + strm_off_clus);
    out.size_bytes = c.priv_rd64(sp + strm_off_dlen);
    stat_exfat_times(&file_e, out);
    if (out.is_directory) {
        out.size_bytes = 0;
    }
    return ok;
}

/// Resolve one name in a FAT12/16/32 directory and decode its entry. The leaf
/// is matched by packed 8.3 name and, failing that, by VFAT long name: the
/// same two-step `priv_open_locked` uses, so `stat` and `open` agree about
/// what exists.
fn stat_fat(handle: *const Mount, path: [*c]const u8, out: *Stat) u16 {
    var parent = std.mem.zeroes(c.dir_loc_t);
    var leaf: [*c]const u8 = null;
    const rerr = c.priv_resolve_parent(handle, path, &parent, &leaf);
    if (rerr != ok) return rerr;
    var name83: [name83_len]u8 = @splat(0);
    const have83: u8 = c.priv_path_to_83(leaf, &name83);
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [dirent_len]u8 = @splat(0);
    var err: u16 = err_not_found;
    if (have83 != 0) {
        err = c.priv_dir_find(handle, &parent, &name83, &lba, &off, &entry);
    }
    if (err == err_not_found) {
        // Same long-name fallback as priv_open_locked: a leaf that is not
        // 8.3-representable still resolves through its VFAT chain.
        err = c.priv_dir_find_long(handle, &parent, leaf, &lba, &off, &entry);
    }
    if (err != ok) return err;
    entry_to_stat(&entry, out);
    return ok;
}

/// Look a path up: the guarded body of `ra8_fs_stat`. Validates the
/// arguments, answers the volume root out of the mount geometry (the root has
/// no directory entry to read), and otherwise hands off to the exFAT or the
/// FAT lookup. Nothing is opened on any path, so no file-table slot is
/// consumed and a directory never has to be mistaken for a zero-byte file to
/// be reported at all.
fn stat_locked(handle: ?*const Mount, path: [*c]const u8, out: ?*Stat) u16 {
    if (handle == null) return err_null_ptr;
    if (path == null) return err_null_ptr;
    if (out == null) return err_null_ptr;
    if (handle.?.in_use == 0) return err_invalid_state;
    out.?.* = std.mem.zeroes(Stat);
    if (path_is_root(path)) {
        stat_root(handle.?, out.?);
        return ok;
    }
    if (handle.?.type == c.k_ra8_fs_type_exfat) {
        return stat_exfat(handle.?, path, out.?);
    }
    return stat_fat(handle.?, path, out.?);
}

pub export fn ra8_fs_stat(handle: ?*const Mount, path: [*c]const u8, out: ?*Stat) callconv(.c) u16 {
    c.priv_lock_acquire();
    const err: u16 = stat_locked(handle, path, out);
    c.priv_lock_release();
    return err;
}
