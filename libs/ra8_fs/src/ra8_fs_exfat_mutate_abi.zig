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

inline fn rd16(p: [*c]const u8, at: u32) u16 {
    return @as(u16, p[at]) | (@as(u16, p[at + 1]) << 8);
}

/// Assemble a file's directory set under a new name, keeping its data.
/// Starts from the file's current File + Stream entries so every field that
/// is NOT the name rides across untouched: the create / modify timestamps,
/// the attributes, FirstCluster, DataLength, ValidDataLength and the
/// secondary flags. Overwrites exactly what a rename changes -- the File
/// entry's SecondaryCount, the Stream entry's NameLength + NameHash, and a
/// run of Name entries carrying fifteen UTF-16 units apiece. The access
/// stamp is refreshed just before the SetChecksum is recomputed over the
/// whole set. A name of 16+ units needs more than one Name entry, so the
/// built set can be longer OR shorter than the one on the volume; that is
/// why the placement below decides where the result lands.
pub fn internal_exfat_build_rename_set(set: [*c]u8, file_e: [*c]const u8, strm_e: [*c]const u8, new_name: [*c]const u16, new_len: u32) u32 {
    const name_entries: u32 = (new_len + name_per_entry - 1) / name_per_entry;
    const sec_count: u32 = 1 + name_entries;
    const total: u32 = (1 + sec_count) * entry_bytes;
    var i: u32 = 0;
    while (i < total) : (i += 1) {
        set[i] = 0;
    }
    @memcpy(set[0..entry_bytes], file_e[0..entry_bytes]);
    @memcpy(set[entry_bytes .. entry_bytes * 2], strm_e[0..entry_bytes]);
    set[off_file_secnt] = @truncate(sec_count);
    const strm = set[entry_bytes .. entry_bytes * 2];
    strm[strm_off_nlen] = @truncate(new_len);
    priv_wr16(&strm[hash_off], priv_exfat_name_hash(new_name, new_len));
    var n: u32 = 0;
    while (n < name_entries) : (n += 1) {
        const ne = set[(2 + n) * entry_bytes .. (3 + n) * entry_bytes];
        ne[0] = entry_name;
        var ch: u32 = 0;
        while (ch < name_per_entry) : (ch += 1) {
            const p: u32 = (n * name_per_entry) + ch;
            if (p < new_len) {
                priv_wr16(&ne[name_off + (ch * 2)], new_name[p]);
            }
        }
    }
    // Access stamp only, and before the checksum that covers it. Same
    // reasoning as the FAT rename: the name moved, the bytes did not, so
    // LastModified must not move or every backup tool concludes the file
    // changed.
    priv_exfat_file_stamp_access(set);
    priv_wr16(&set[csum_off], priv_exfat_set_checksum(set, total));
    return total;
}

/// Resolve a rename's two paths to one shared parent plus two leaves.
/// Resolves both parents and requires them to be the same directory: this
/// rename rewrites an entry set where it lies, so it cannot move one
/// between directories. Both leaves must be real names -- the volume root
/// is neither renameable nor a legal destination.
pub fn internal_exfat_rename_prepare(m: [*c]const c.ra8_fs_mount_t, old_path: [*c]const u8, new_path: [*c]const u8, out_parent: [*c]c.exfat_dir_t, out_old: [*c][*c]const u8, out_new: [*c][*c]const u8) u16 {
    var op: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var ol: [*c]const u8 = null;
    const e1 = priv_exfat_resolve_parent(m, old_path, &op, &ol);
    if (e1 != ok) {
        return e1;
    }
    var np: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var nl: [*c]const u8 = null;
    const e2 = priv_exfat_resolve_parent(m, new_path, &np, &nl);
    if (e2 != ok) {
        return e2;
    }
    if (op.cluster != np.cluster) {
        return not_supported;
    }
    if (priv_strlen(ol) == 0) {
        return invalid_arg;
    }
    if (priv_strlen(nl) == 0) {
        return invalid_arg;
    }
    out_parent[0] = op;
    out_old[0] = ol;
    out_new[0] = nl;
    return ok;
}

/// Write a rebuilt rename set, in place when it fits, else by relocation.
/// When the new set has the SAME entry count as the old one, every slot is
/// rewritten where it already sits -- the cheap path a same-length rename
/// keeps. Otherwise the set changed length: a fresh run of new_count free
/// slots is located (growing the directory if it must), the whole set is
/// written there, and only THEN is the old set retired by clearing the
/// in-use bit of each of its type bytes. That order is the crash-safe one:
/// the new set names the same clusters as the old, so a failure after the
/// write but before the drop leaves two names for one file -- a transient
/// the next scan resolves -- whereas dropping first would orphan the
/// clusters if the write never completed.
pub fn internal_exfat_place_rename(m: [*c]const c.ra8_fs_mount_t, parent: [*c]const c.exfat_dir_t, pos: [*c]const c.exfat_setpos_t, old_count: u32, set: [*c]const u8, bytes: u32) u16 {
    const new_count: u32 = bytes / entry_bytes;
    if (new_count == old_count) {
        var k: u32 = 0;
        while (k < new_count) : (k += 1) {
            const e = internal_exfat_put_entry(m, &pos[k], set[k * entry_bytes ..][0..entry_bytes]);
            if (e != ok) {
                return e;
            }
        }
        return ok;
    }
    var nclus: u32 = 0;
    var nidx: u32 = 0;
    const se = priv_exfat_find_dir_space(m, parent, new_count, &nclus, &nidx);
    if (se != ok) {
        return se;
    }
    const we = priv_exfat_write_dir_set(m, nclus, nidx, set, bytes);
    if (we != ok) {
        return we;
    }
    return priv_exfat_drop_set(m, pos, old_count);
}

/// Rename a file or directory within one parent directory. The new leaf is
/// converted to UTF-16 up front: the built Name entries and the NameHash
/// both count CODE UNITS, and a UTF-8 argument of the same unit count can
/// be three times as many bytes. A name over the cap is an argument fault,
/// not a full disk. The old leaf stays UTF-8: find_set converts it itself,
/// and the entry count it reports is all the placement below needs. The
/// read-only attribute is honored from the File entry find_set read back,
/// and an existing entry under the new name refuses the rename outright.
pub export fn priv_exfat_rename(m: [*c]const c.ra8_fs_mount_t, old_path: [*c]const u8, new_path: [*c]const u8) callconv(.c) u16 {
    var parent: c.exfat_dir_t = std.mem.zeroes(c.exfat_dir_t);
    var old_name: [*c]const u8 = null;
    var new_name: [*c]const u8 = null;
    const pe = internal_exfat_rename_prepare(m, old_path, new_path, &parent, &old_name, &new_name);
    if (pe != ok) {
        return pe;
    }
    var new_units: [name_cap]u16 = @splat(0);
    var new_len: u32 = 0;
    const nue = priv_exfat_needle_units(m, new_name, &new_units, &new_len, invalid_arg);
    if (nue != ok) {
        return nue;
    }
    var e_strm: [entry_bytes]u8 = @splat(@splat(0));
    var e_attr: u8 = 0;
    if (priv_exfat_find(m, &parent, new_name, &e_strm, &e_attr) == ok) {
        return exists;
    }
    var pos: [set_max_entries]c.exfat_setpos_t = std.mem.zeroes([set_max_entries]c.exfat_setpos_t);
    var count: u32 = 0;
    var file_e: [entry_bytes]u8 = @splat(@splat(0));
    var strm_e: [entry_bytes]u8 = @splat(@splat(0));
    const fe = priv_exfat_find_set(m, &parent, old_name, &pos, set_max_entries, &count, &file_e, &strm_e);
    if (fe != ok) {
        return fe;
    }
    // Honor the read-only attribute. file_e is the File entry read back by
    // find_set above, so its FileAttributes low byte carries the read-only
    // bit; a read-only file is refused before the rename set is rebuilt or
    // placed.
    if ((file_e[off_file_attr] & attr_read_only) != 0) {
        return access_denied;
    }
    var set: [max_set_bytes]u8 = @splat(@splat(0));
    const bytes = internal_exfat_build_rename_set(&set, &file_e, &strm_e, &new_units, new_len);
    return internal_exfat_place_rename(m, &parent, &pos, count, &set, bytes);
}

/// Consume a set's Name entries and assemble the name as UTF-8.
/// Reads the sc - 1 secondaries that follow the Stream entry, collecting
/// whole UTF-16 code units, then converts the lot to UTF-8 in one step.
/// Non-Name secondaries are consumed and skipped so the caller's cursor
/// stays aligned. Collecting units and converting once is what makes the
/// conversion possible at all: a variable-width encoding cannot be
/// produced one fixed-width unit at a time into a byte-indexed buffer.
/// A set whose units cannot be expressed in UTF-8 at all -- an unpaired
/// surrogate, which no conforming writer produces -- yields an empty name,
/// and listdir reports nothing for that entry rather than a name that
/// would not re-open the file.
pub fn internal_exfat_gather_name(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, sc: u32, nlen: u32, name: [*c]u8, cap: u32) u16 {
    var units: [name_cap]u16 = @splat(0);
    var got: u32 = 0;
    name[0] = 0;
    var k: u32 = 1;
    while (k < sc) : (k += 1) {
        var ne: [entry_bytes]u8 = @splat(@splat(0));
        const r = priv_exfat_next_entry(m, cur, &ne);
        if (r != ok) {
            return r;
        }
        if (ne[0] != entry_name) {
            continue;
        }
        var ch: u32 = 0;
        while (ch < name_per_entry) : (ch += 1) {
            const p: u32 = ((k - 1) * name_per_entry) + ch;
            if (p >= nlen) {
                break;
            }
            if (got < name_cap) {
                units[got] = rd16(&ne, name_off + (ch * 2));
                got += 1;
            }
        }
    }
    if (priv_utf16_to_utf8(&units, got, name, cap) != ok) {
        name[0] = 0;
    }
    return ok;
}

/// Copy one File entry set into a stable directory value. Reads the Stream
/// entry, decides the size a directory reports (0, not its allocation),
/// gathers the name as UTF-8 and fills out -- unless the name is one no
/// UTF-8 string encodes, which is reported as absent rather than as
/// mojibake.
pub fn internal_exfat_list_emit(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, e: [*c]const u8, out: [*c]c.ra8_fs_dirent_t, out_entry: [*c]bool) u16 {
    const sc: u32 = e[off_file_secnt];
    const attr: u8 = e[off_file_attr];
    var strm: [entry_bytes]u8 = @splat(@splat(0));
    const r = priv_exfat_next_entry(m, cur, &strm);
    if (r != ok) {
        return r;
    }
    if (strm[0] != entry_stream) {
        return ok;
    }
    // A directory's exFAT DataLength is its ALLOCATION, not a byte count,
    // so reporting it as a size would tell every caller that an empty
    // folder held a cluster's worth of bytes. FAT reports 0 for a directory
    // and so does ra8_fs_stat(); this is the third place that has to agree.
    var size: u64 = rd64(&strm, strm_off_dlen);
    if ((attr & attr_directory) != 0) {
        size = 0;
    }
    const nlen: u32 = strm[strm_off_nlen];
    var name: [name_u8_cap]u8 = @splat(@splat(0));
    const r2 = internal_exfat_gather_name(m, cur, sc, nlen, &name, name_u8_cap);
    if (r2 != ok) {
        return r2;
    }
    if (name[0] == 0) {
        return ok; // units no UTF-8 string encodes: report nothing, not a lie
    }
    var len: usize = 0;
    while (name[len] != 0) : (len += 1) {}
    @memcpy(out[0].name[0 .. len + 1], name[0 .. len + 1]);
    out[0].attr = attr;
    out[0].size_bytes = size;
    out_entry[0] = true;
    return ok;
}

/// Advance a directory cursor to the next visible entry. Walks until a File
/// entry set yields a real name or the directory ends; anything that is not
/// a File entry (deleted, volume-label, up-case, bitmap, GUID) is skipped,
/// and a set whose name cannot be UTF-8 is skipped too. Exhausting the
/// scan limit without reaching an end-of-directory marker is a protocol
/// error: a directory that big is not a directory we wrote.
pub export fn priv_exfat_dir_next(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]c.ra8_fs_dirent_t, out_entry: [*c]bool) callconv(.c) u16 {
    out_entry[0] = false;
    while (cur[0].scanned < scan_limit) {
        var entry: [entry_bytes]u8 = @splat(@splat(0));
        const err = priv_exfat_next_entry(m, cur, &entry);
        if (err == not_found) {
            return ok;
        }
        if (err != ok) {
            return err;
        }
        if (entry[0] == entry_eod) {
            return ok;
        }
        if (entry[0] != entry_file) {
            continue;
        }
        const e2 = internal_exfat_list_emit(m, cur, &entry, out, out_entry);
        if (e2 != ok) {
            return e2;
        }
        if (out_entry[0]) {
            return ok;
        }
    }
    return invalid_size;
}

/// List a directory through the caller's callback: one stable dirent value
/// per visible entry, walking until the directory ends or the backend
/// fails. The callback runs on every emitted entry; an error or an
/// exhausted cursor ends the walk and is returned as-is.
pub export fn priv_exfat_listdir(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, cb: c.ra8_fs_listdir_cb_t, ctx: ?*anyopaque) callconv(.c) u16 {
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    priv_exfat_cursor_init(dir, &cur);
    while (true) {
        var entry: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
        var present: bool = false;
        const err = priv_exfat_dir_next(m, &cur, &entry, &present);
        if (err != ok or !present) {
            return err;
        }
        cb.?(&entry.name, entry.attr, entry.size_bytes, ctx);
    }
}
