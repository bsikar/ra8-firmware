//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_exfat_open_write`: open an exFAT file for writing or appending,
//! creating it when the leaf is missing.
//!
//! An existing file must be a writable regular file: not a directory, not
//! read-only, and with an entry set short enough to rewrite in place
//! (`k_exfat_set_writable`). Write mode frees its clusters and truncates;
//! append mode surveys the allocation so the stream can extend it. A missing
//! leaf is linked into the parent as a new, empty set. Path resolution, name
//! conversion, the set lookup, linking, cluster freeing and the set flush stay
//! in C, reached through `fs_c.zig`.
//!
//! Bounded loops: the FAT survey follows at most `count_of_clusters` links.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const File = c.ra8_fs_file_t;
pub const Mount = c.ra8_fs_mount_t;

pub const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_access_denied: u16 = c.k_ra8_err_access_denied;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const err_not_found: u16 = c.k_ra8_err_not_found;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const entry_bytes = c.k_exfat_entry_bytes;
const set_max = c.k_exfat_set_max_entries;
const name_cap = c.k_exfat_name_cap;

fn rd32(p: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, p[off..][0..4], .little);
}

fn rd64(p: []const u8, off: usize) u64 {
    return std.mem.readInt(u64, p[off..][0..8], .little);
}

/// A fresh, empty handle on the set at `head`.
fn seed(f: *File, m: *Mount, head: *const c.exfat_setpos_t, count: u32, mode: c.ra8_fs_mode_t) void {
    f.mount = m;
    f.first_cluster = 0;
    f.cur_cluster = 0;
    f.walk_cache_idx = 0;
    f.walk_cache_cluster = 0;
    f.size_bytes = 0;
    f.offset = 0;
    f.dir_entry_lba = 0;
    f.dir_entry_idx = 0;
    f.valid_bytes = 0;
    f.entry_set_cluster = head.cluster;
    f.entry_set_index = head.index;
    f.entry_set_count = @intCast(count);
    f.alloc_clusters = 0;
    f.tail_cluster = 0;
    f.mode = mode;
    f.in_use = 1;
    f.no_fat_chain = 1;
    f.dirty = 0;
}

fn release(f: *File) void {
    f.in_use = 0;
    f.mount = null;
}

/// Counts the clusters a file owns and finds its tail, so appends extend it.
pub fn surveyAlloc(f: *File) u16 {
    const m: *const Mount = f.mount;
    const cbytes = c.priv_cluster_bytes(m);
    if (f.first_cluster < first_data or f.size_bytes == 0) {
        f.first_cluster = 0;
        f.alloc_clusters = 0;
        f.tail_cluster = 0;
        f.no_fat_chain = 1;
        return ok;
    }
    if (f.no_fat_chain != 0) {
        f.alloc_clusters = @intCast((f.size_bytes + cbytes - 1) / cbytes);
        f.tail_cluster = f.first_cluster + f.alloc_clusters - 1;
        return ok;
    }
    var clus = f.first_cluster;
    var owned: u32 = 1;
    var guard: u32 = 0;
    while (guard < m.count_of_clusters) : (guard += 1) {
        var next: u32 = 0;
        const e = c.priv_fat_get(m, clus, &next);
        if (e != ok) return e;
        if (c.priv_is_eoc(m, next) != 0) break;
        if (next < first_data) return err_protocol;
        clus = next;
        owned += 1;
    }
    f.alloc_clusters = owned;
    f.tail_cluster = clus;
    return ok;
}

fn truncate(f: *File, strm: [*]const u8) u16 {
    const e = c.priv_exfat_free_clusters(f.mount, strm);
    if (e != ok) return e;
    f.first_cluster = 0;
    f.cur_cluster = 0;
    f.walk_cache_idx = 0;
    f.walk_cache_cluster = 0;
    f.size_bytes = 0;
    f.valid_bytes = 0;
    f.offset = 0;
    f.alloc_clusters = 0;
    f.tail_cluster = 0;
    f.no_fat_chain = 1;
    return c.priv_exfat_flush_set(f);
}

fn checkFound(count: u32, file_e: []const u8) u16 {
    const attr = file_e[c.k_exfat_off_file_attr];
    if (attr & c.k_exfat_attr_directory != 0) return err_invalid_arg;
    if (attr & c.k_exfat_attr_read_only != 0) return err_access_denied;
    if (count < c.k_exfat_set_min_entries) return err_protocol;
    if (count > c.k_exfat_set_writable) return err_not_supported;
    return ok;
}

fn openFound(m: *Mount, mode: c.ra8_fs_mode_t, head: *const c.exfat_setpos_t, count: u32, file_e: []const u8, strm: []const u8, out: *?*File) u16 {
    var e = checkFound(count, file_e);
    if (e != ok) return e;
    const f: *File = c.priv_alloc_file_slot() orelse return err_no_mem;
    seed(f, m, head, count, mode);
    f.first_cluster = rd32(strm, c.k_exfat_strm_off_clus);
    f.size_bytes = @intCast(rd64(strm, c.k_exfat_strm_off_dlen));
    // ValidDataLength is a prefix of DataLength (spec sec 7.4.5); clamp a bad one.
    f.valid_bytes = @intCast(@min(rd64(strm, c.k_exfat_off_strm_valid), f.size_bytes));
    f.no_fat_chain = if (strm[c.k_exfat_strm_off_flags] & c.k_exfat_secflag_no_fat != 0) 1 else 0;
    if (mode == c.k_ra8_fs_mode_write) {
        e = truncate(f, strm.ptr);
    } else {
        e = surveyAlloc(f);
        f.cur_cluster = f.first_cluster;
        f.walk_cache_cluster = f.first_cluster;
        f.offset = f.size_bytes;
    }
    if (e != ok) {
        release(f);
        return e;
    }
    out.* = f;
    return ok;
}

fn openCreated(m: *Mount, dir: *const c.exfat_dir_t, name: [*]const u16, nlen: u32, mode: c.ra8_fs_mode_t, out: *?*File) u16 {
    const f: *File = c.priv_alloc_file_slot() orelse return err_no_mem;
    var head = std.mem.zeroes(c.exfat_setpos_t);
    var count: u32 = 0;
    const e = c.priv_exfat_link(m, dir, name, nlen, &head, &count);
    if (e != ok) {
        release(f);
        return e;
    }
    seed(f, m, &head, count, mode);
    out.* = f;
    return ok;
}

/// Opens `path` for `mode` (write or append), creating the leaf if missing.
pub export fn priv_exfat_open_write(m: *Mount, path: [*:0]const u8, mode: c.ra8_fs_mode_t, out: *?*File) callconv(.C) u16 {
    var parent = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    var e = c.priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) return e;
    var name: [name_cap]u16 = @splat(0);
    var nlen: u32 = 0;
    e = c.priv_exfat_name_to_units(m, leaf, &name, &nlen);
    if (e == err_no_mem) return err_invalid_arg; // a name too long to store
    if (e != ok) return e;
    if (nlen == 0) return err_invalid_arg;
    var pos = std.mem.zeroes([set_max]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_e: [entry_bytes]u8 = @splat(0);
    var strm: [entry_bytes]u8 = @splat(0);
    e = c.priv_exfat_find_set(m, &parent, leaf, &pos, set_max, &count, &file_e, &strm);
    if (e == ok) return openFound(m, mode, &pos[0], count, &file_e, &strm, out);
    if (e != err_not_found) return e;
    return openCreated(m, &parent, &name, nlen, mode, out);
}
