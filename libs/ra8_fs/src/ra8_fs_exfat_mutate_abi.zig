//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT mutation helpers, first half (RA8FW-1124): locate a directory-entry
//! set on the volume, free a file's clusters and their bitmap bits, rewrite
//! or retire the set's entries, and unlink a file by name. Ported from the
//! first half of `ra8_fs_fat_exfat_mutate.c`; the rename and listdir half
//! stays C until the next slice.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const not_found: u16 = c.k_ra8_err_not_found;
const no_mem: u16 = c.k_ra8_err_no_mem;
const invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const access_denied: u16 = c.k_ra8_err_access_denied;
const protocol_error: u16 = c.k_ra8_err_protocol_error;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const entry_bytes: u32 = @intCast(c.k_exfat_entry_bytes);
const name_cap: u32 = @intCast(c.k_exfat_name_cap);
const name_per_entry: u32 = @intCast(c.k_exfat_name_per_entry);
const scan_limit: u32 = @intCast(c.k_exfat_scan_limit);
const set_max_entries: u32 = @intCast(c.k_exfat_set_max_entries);

const entry_eod: u8 = @intCast(c.k_exfat_entry_eod);
const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const entry_stream: u8 = @intCast(c.k_exfat_entry_stream);
const entry_name: u8 = @intCast(c.k_exfat_entry_name);
const inuse_bit: u8 = @intCast(c.k_exfat_inuse_bit);
const secflag_no_fat: u8 = @intCast(c.k_exfat_secflag_no_fat);
const attr_directory: u8 = @intCast(c.k_exfat_attr_directory);
const attr_read_only: u8 = @intCast(c.k_exfat_attr_read_only);

const bit_shift: u5 = @intCast(c.k_exfat_bit_shift);
const bit_mask: u32 = @intCast(c.k_exfat_bit_mask);
const strm_off_flags: u32 = @intCast(c.k_exfat_strm_off_flags);
const strm_off_nlen: u32 = @intCast(c.k_exfat_strm_off_nlen);
const strm_off_clus: u32 = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: u32 = @intCast(c.k_exfat_strm_off_dlen);
const off_file_secnt: u32 = @intCast(c.k_exfat_off_file_secnt);
const off_file_attr: u32 = @intCast(c.k_exfat_off_file_attr);

// C helpers that stay C for now (the bytes, alloc and dir halves).
extern fn priv_sec_io() callconv(.c) [*c]u8;
extern fn priv_exfat_bmp_switch(m: [*c]const c.ra8_fs_mount_t, lba: u64, loaded: [*c]u64, sec: [*c]u8) callconv(.c) u16;
extern fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16;
extern fn priv_alloc_hint_lower(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) void;
extern fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_exfat_bitmap_lba(m: [*c]const c.ra8_fs_mount_t, out_lba: [*c]u64) callconv(.c) u16;
extern fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8;
extern fn priv_exfat_next_entry(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]u8) callconv(.c) u16;
extern fn priv_exfat_name_chunk_eq(entry: [*c]const u8, name: [*c]const u16, pos: u32, nlen: u32) callconv(.c) u32;
extern fn priv_exfat_needle_units(m: [*c]const c.ra8_fs_mount_t, name: [*c]const u8, out: [*c]u16, out_units: [*c]u32, too_long: u16) callconv(.c) u16;
extern fn priv_exfat_cursor_init(dir: [*c]const c.exfat_dir_t, out: [*c]c.exfat_cursor_t) callconv(.c) void;
extern fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16;
extern fn priv_exfat_resolve_parent(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8, out_parent: [*c]c.exfat_dir_t, out_leaf: [*c][*c]const u8) callconv(.c) u16;
extern fn priv_strlen(s: [*c]const u8) callconv(.c) u32;

inline fn rd32(p: [*c]const u8, at: u32) u32 {
    return @as(u32, p[at]) | (@as(u32, p[at + 1]) << 8) |
        (@as(u32, p[at + 2]) << 16) | (@as(u32, p[at + 3]) << 24);
}

inline fn rd64(p: [*c]const u8, at: u32) u64 {
    return @as(u64, rd32(p, at)) | (@as(u64, rd32(p, at + 4)) << 32);
}

/// Clear `count` bits for `clus` and its successors in the cluster bitmap,
/// keeping the loaded-sector cache across the run and writing the sector
/// back once at the end. Every freed cluster pulls the allocation hint back
/// to the space just released, or the next create would step over it and
/// only find it after a full rescan.
pub export fn priv_exfat_bitmap_clear(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, clus: u32, count: u32) callconv(.c) u16 {
    var loaded: u64 = std.math.maxInt(u64);
    const sec = priv_sec_io();
    var k: u32 = 0;
    while (k < count) : (k += 1) {
        const idx: u32 = (clus -% first_data) +% k;
        const byte_off: u32 = idx >> bit_shift;
        const lba: u64 = bmp_lba +% @as(u64, byte_off / priv_bps(m));
        const byte: u32 = byte_off % priv_bps(m);
        const bit: u5 = @intCast(idx & bit_mask);
        const e = priv_exfat_bmp_switch(m, lba, &loaded, sec);
        if (e != ok) {
            return e;
        }
        sec[byte] &= ~@as(u8, @truncate(@as(u32, 1) << bit));
        priv_alloc_hint_lower(m, idx +% first_data);
    }
    return priv_write_sector(m, loaded, sec);
}

/// Consume a set's secondary entries, recording positions and matching.
/// Reads the `sc` secondaries following a File entry, snapshots each entry's
/// position into `pos` starting at slot 1, and checks the set against
/// `name`: the first secondary must be a Stream entry with the right
/// NameLength, the rest must be Name entries whose UTF-16 chunks equal the
/// name. The cursor always consumes all `sc` entries so the caller's walk
/// stays aligned. Helper of `priv_exfat_find_set`.
pub fn internal_exfat_take_set(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, name: [*c]const u16, nlen: u32, sc: u32, pos: [*c]c.exfat_setpos_t, strm_copy: [*c]u8, out_match: [*c]u8) u16 {
    var matched: u8 = 1;
    var k: u32 = 0;
    while (k < sc) : (k += 1) {
        var se: [entry_bytes]u8 = @splat(@splat(0));
        const r = priv_exfat_next_entry(m, cur, &se);
        if (r != ok) {
            return r;
        }
        // AFTER the read: next_entry may cross a cluster boundary before
        // reading, so the entry lives at the cursor's CURRENT cluster, one
        // slot back (it has already advanced past what it read).
        pos[1 + k] = .{ .cluster = cur[0].cluster, .index = cur[0].entry_in_cluster -% 1 };
        if (k == 0) {
            if (se[0] != entry_stream) {
                matched = 0;
                continue;
            }
            if (@as(u32, se[strm_off_nlen]) != nlen) {
                matched = 0;
                continue;
            }
            @memcpy(strm_copy[0..entry_bytes], &se);
            continue;
        }
        if (matched == 0) {
            continue;
        }
        if (se[0] != entry_name) {
            matched = 0;
            continue;
        }
        const cpos: u32 = (k - 1) * name_per_entry;
        if (priv_exfat_name_chunk_eq(&se, name, cpos, nlen) == 0) {
            matched = 0;
        }
    }
    out_match[0] = matched;
    return ok;
}

/// Test one File entry set against a needle, recording its positions. On a
/// match fills `pos` and `out_count`; otherwise reports not_found so the
/// caller keeps scanning. Helper of `priv_exfat_find_set`.
pub fn internal_exfat_try_set(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, at: c.exfat_setpos_t, e: [*c]const u8, need: [*c]const u16, nlen: u32, pos: [*c]c.exfat_setpos_t, max_pos: u32, out_count: [*c]u32, file_copy: [*c]u8, strm_copy: [*c]u8) u16 {
    const sc: u32 = e[off_file_secnt];
    const total: u32 = 1 +% sc;
    if (total > max_pos) {
        return no_mem;
    }
    pos[0] = at;
    @memcpy(file_copy[0..entry_bytes], e[0..entry_bytes]);
    var matched: u8 = 0;
    const r = internal_exfat_take_set(m, cur, need, nlen, sc, pos, strm_copy, &matched);
    if (r != ok) {
        return r;
    }
    if (matched == 1) {
        out_count[0] = total;
        return ok;
    }
    return not_found;
}

/// One aligned walk of a directory, locating the File entry set whose name
/// equals `path`: leading slashes are stripped so "/name" matches as FAT
/// does, every non-File entry is skipped, and a set is consumed whole before
/// it is judged. The set's positions land in `pos` (File entry first), the
/// File and Stream entries land in the copies, and `out_count` carries
/// 1 + SecondaryCount.
pub export fn priv_exfat_find_set(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, path: [*c]const u8, pos: [*c]c.exfat_setpos_t, max_pos: u32, out_count: [*c]u32, file_copy: [*c]u8, strm_copy: [*c]u8) callconv(.c) u16 {
    var p = path;
    while (p[0] == '/') {
        p += 1;
    }
    var need: [name_cap]u16 = @splat(@splat(0));
    var nlen: u32 = 0;
    const ne = priv_exfat_needle_units(m, p, &need, &nlen, not_found);
    if (ne != ok) {
        return ne;
    }
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    priv_exfat_cursor_init(dir, &cur);
    while (cur.scanned < scan_limit) {
        var e: [entry_bytes]u8 = @splat(@splat(0));
        var r = priv_exfat_next_entry(m, &cur, &e);
        if (r != ok) {
            return r;
        }
        // AFTER the read (see take_set): the File entry lives at the cursor's
        // current cluster, one slot back, so a set whose File entry is the
        // first of a grown cluster is recorded where it truly is.
        const at: c.exfat_setpos_t = .{ .cluster = cur.cluster, .index = cur.entry_in_cluster -% 1 };
        if (e[0] == entry_eod) {
            return not_found;
        }
        if (e[0] != entry_file) {
            continue;
        }
        r = internal_exfat_try_set(m, &cur, at, &e, &need, nlen, pos, max_pos, out_count, file_copy, strm_copy);
        if (r != not_found) {
            return r;
        }
    }
    return protocol_error;
}

/// Rewrite one 32-byte directory entry at a recorded position. Thin wrapper
/// over `priv_exfat_write_dir_set` for a single entry; the rename half
/// (still C) drives it through the header prototype.
pub export fn internal_exfat_put_entry(m: [*c]const c.ra8_fs_mount_t, where: [*c]const c.exfat_setpos_t, entry: [*c]const u8) callconv(.c) u16 {
    return priv_exfat_write_dir_set(m, where[0].cluster, where[0].index, entry, entry_bytes);
}

/// Free a file's clusters in the allocation bitmap only: the whole run when
/// the stream says NoFatChain (the full 64-bit DataLength is read, since
/// only the low word under-counted a >4 GiB file's clusters), otherwise the
/// FAT chain cluster by cluster until an end-of-chain marker. A first
/// cluster below the data region or a zero DataLength has nothing to free.
pub export fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16 {
    const first: u32 = rd32(strm, strm_off_clus);
    const size: u64 = rd64(strm, strm_off_dlen);
    if (first < first_data) {
        return ok;
    }
    if (size == 0) {
        return ok;
    }
    var bmp_lba: u64 = 0;
    const e = priv_exfat_bitmap_lba(m, &bmp_lba);
    if (e != ok) {
        return e;
    }
    const cluster_bytes = priv_cluster_bytes(m);
    const nofat: u8 = if ((strm[strm_off_flags] & secflag_no_fat) != 0) 1 else 0;
    if (nofat == 1) {
        const count: u32 = @truncate((size +% @as(u64, cluster_bytes) -% 1) / cluster_bytes);
        return priv_exfat_bitmap_clear(m, bmp_lba, first, count);
    }
    var clus = first;
    var guard: u32 = 0;
    while (guard < m[0].count_of_clusters) : (guard += 1) {
        const e2 = priv_exfat_bitmap_clear(m, bmp_lba, clus, 1);
        if (e2 != ok) {
            return e2;
        }
        var next: u32 = 0;
        const e3 = priv_fat_get(m, clus, &next);
        if (e3 != ok) {
            return e3;
        }
        if (priv_is_eoc(m, next) != 0) {
            return ok;
        }
        clus = next;
    }
    return protocol_error;
}

/// Mark every entry of a located set as deleted: each entry is re-read at
/// its recorded position (a single-entry read; contig_end is irrelevant
/// because this cursor never advances past the entry it was aimed at) and
/// rewritten with the in-use bit cleared.
pub export fn priv_exfat_drop_set(m: [*c]const c.ra8_fs_mount_t, pos: [*c]const c.exfat_setpos_t, count: u32) callconv(.c) u16 {
    var k: u32 = 0;
    while (k < count) : (k += 1) {
        var one: c.exfat_cursor_t = .{
            .cluster = pos[k].cluster,
            .entry_in_cluster = pos[k].index,
            .scanned = 0,
            .contig_end = 0,
        };
        var entry: [entry_bytes]u8 = @splat(@splat(0));
        const e = priv_exfat_next_entry(m, &one, &entry);
        if (e != ok) {
            return e;
        }
        entry[0] &= ~inuse_bit;
        const e2 = internal_exfat_put_entry(m, &pos[k], &entry);
        if (e2 != ok) {
            return e2;
        }
    }
    return ok;
}

/// Unlink a file whose entry set was already located in `dir` by name. A
/// directory's entry set looks like a file's, so it is refused before the
/// chain holding every child could be handed to free_clusters -- silent,
/// unrecoverable loss otherwise. The read-only attribute is honored the
/// same way: a denied unlink is a no-op.
pub export fn priv_exfat_unlink_at(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, name: [*c]const u8) callconv(.c) u16 {
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_e: [entry_bytes]u8 = @splat(@splat(0));
    var strm_e: [entry_bytes]u8 = @splat(@splat(0));
    const e = priv_exfat_find_set(m, dir, name, &pos, set_max_entries, &count, &file_e, &strm_e);
    if (e != ok) {
        return e;
    }
    if ((file_e[off_file_attr] & attr_directory) != 0) {
        return invalid_arg;
    }
    if ((file_e[off_file_attr] & attr_read_only) != 0) {
        return access_denied;
    }
    const de = priv_exfat_drop_set(m, &pos, count);
    if (de != ok) {
        return de;
    }
    return priv_exfat_free_clusters(m, &strm_e);
}

/// Unlink a file by path: resolve the parent directory, then refuse the
/// root itself ("/" resolves to an empty leaf, and the root is not a file).
pub export fn priv_exfat_unlink(m: [*c]const c.ra8_fs_mount_t, path: [*c]const u8) callconv(.c) u16 {
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var leaf: [*c]const u8 = null;
    const e = priv_exfat_resolve_parent(m, path, &parent, &leaf);
    if (e != ok) {
        return e;
    }
    if (priv_strlen(leaf) == 0) {
        return invalid_arg;
    }
    return priv_exfat_unlink_at(m, &parent, leaf);
}
