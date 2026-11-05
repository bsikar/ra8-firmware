//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT growth (RA8FW-1118 cluster growth, RA8FW-1119 directory growth):
//! pick the next cluster, turn a contiguous run into a real FAT chain, link
//! a freshly allocated cluster onto a file's or directory's allocation, grow
//! one cluster at a time, and rewrite a grown directory's own Stream entry.
//! RA8FW-1120 adds the stream-write half: resolve a byte offset to a
//! sector, zero-fill an unwritten prefix, write the payload, and flush a
//! file's entry set back to the volume. Ported from
//! `ra8_fs_fat_exfat_stream.c`, now an include stub.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const first_data: u32 = @intCast(c.k_cluster_first_data);

extern fn priv_exfat_bitmap_lba(m: [*c]const c.ra8_fs_mount_t, out_lba: [*c]u64) callconv(.c) u16;
extern fn priv_exfat_bitmap_test(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, clus: u32, out_free: [*c]u8) callconv(.c) u16;
extern fn priv_exfat_bitmap_scan(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, need: u32, out_clus: [*c]u32) callconv(.c) u16;
extern fn priv_exfat_bitmap_mark(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, clus: u32, count: u32) callconv(.c) u16;
extern fn priv_alloc_hint_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) void;
extern fn priv_fat_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, value: u32) callconv(.c) u16;
extern fn priv_eoc_write(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;

/// Take `prefer` when it names a free data cluster; scan from the hint when
/// it is taken or lies outside the data region.
pub export fn internal_exfat_pick_cluster(m: [*c]const c.ra8_fs_mount_t, bmp_lba: u64, prefer: u32, out: [*c]u32) callconv(.c) u16 {
    const past_end: u32 = first_data +% m[0].count_of_clusters;
    if ((prefer >= first_data) and (prefer < past_end)) {
        var is_free: u8 = 0;
        const pe = priv_exfat_bitmap_test(m, bmp_lba, prefer, &is_free);
        if (pe != ok) {
            return pe;
        }
        if (is_free != 0) {
            out[0] = prefer;
            return ok;
        }
    }
    return priv_exfat_bitmap_scan(m, bmp_lba, 1, out);
}

/// Write a real FAT chain over a contiguous run, ending at `next`: every
/// already-allocated cluster points at its successor (the next integer on a
/// contiguous run) and the tail points at `next`. The caller writes `next`'s
/// end-of-chain marker.
pub export fn internal_exfat_materialize_run(m: [*c]const c.ra8_fs_mount_t, first: u32, alloc: u32, tail: u32, next: u32) callconv(.c) u16 {
    var i: u32 = 0;
    while (i + 1 < alloc) : (i += 1) {
        const here = first + i;
        const e = priv_fat_set(m, here, here + 1);
        if (e != ok) {
            return e;
        }
    }
    return priv_fat_set(m, tail, next);
}

/// Attach a freshly allocated cluster to the file's allocation. Nothing to
/// link while the file owns no clusters or the new cluster continues a
/// NoFatChain run; a broken run is converted to a real chain first, and a
/// chained allocation just links tail to `next`.
pub export fn internal_exfat_link_cluster(file: [*c]c.ra8_fs_file_t, next: u32) callconv(.c) u16 {
    if (file[0].alloc_clusters == 0) {
        return ok;
    }
    if ((file[0].no_fat_chain != 0) and (next == file[0].tail_cluster + 1)) {
        return ok;
    }
    if (file[0].no_fat_chain != 0) {
        const e = internal_exfat_materialize_run(
            file[0].mount,
            file[0].first_cluster,
            file[0].alloc_clusters,
            file[0].tail_cluster,
            next,
        );
        if (e != ok) {
            return e;
        }
        file[0].no_fat_chain = 0;
    } else {
        const e = priv_fat_set(file[0].mount, file[0].tail_cluster, next);
        if (e != ok) {
            return e;
        }
    }
    return priv_fat_set(file[0].mount, next, priv_eoc_write(file[0].mount));
}

/// Add one cluster to a file's allocation: mark it in the bitmap FIRST and
/// only then link it, so a mid-grow failure leaks a cluster (a later fsck
/// reclaims it) instead of leaving the file pointing at a "free" cluster.
pub export fn internal_exfat_grow_one(file: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    const m = file[0].mount;
    var bmp_lba: u64 = 0;
    var e = priv_exfat_bitmap_lba(m, &bmp_lba);
    if (e != ok) {
        return e;
    }
    const prefer: u32 = if (file[0].alloc_clusters > 0) file[0].tail_cluster + 1 else 0;
    var next: u32 = 0;
    e = internal_exfat_pick_cluster(m, bmp_lba, prefer, &next);
    if (e != ok) {
        return e;
    }
    e = priv_exfat_bitmap_mark(m, bmp_lba, next, 1);
    if (e != ok) {
        return e;
    }
    priv_alloc_hint_set(m, next + 1);
    e = internal_exfat_link_cluster(file, next);
    if (e != ok) {
        return e;
    }
    if (file[0].alloc_clusters == 0) {
        file[0].first_cluster = next;
        file[0].cur_cluster = next;
        file[0].walk_cache_idx = 0;
        file[0].walk_cache_cluster = next;
    }
    file[0].tail_cluster = next;
    file[0].alloc_clusters += 1;
    return ok;
}

/// Grow the file until it owns `need` clusters.
pub export fn priv_exfat_ensure_clusters(file: [*c]c.ra8_fs_file_t, need: u32) callconv(.c) u16 {
    while (file[0].alloc_clusters < need) {
        const e = internal_exfat_grow_one(file);
        if (e != ok) {
            return e;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Directory growth (RA8FW-1119): the same allocation with the two things a
// directory needs that a mid-write file does not -- the new cluster is ZEROED
// before it is linked, and the directory's OWN Stream entry in its parent is
// rewritten so DataLength and NoFatChain keep describing the run after the
// mount that grew it is gone.
// ---------------------------------------------------------------------------

extern fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8;
extern fn priv_exfat_zero_cluster(m: [*c]const c.ra8_fs_mount_t, clus: u32) callconv(.c) u16;
extern fn priv_exfat_seal_cluster(m: [*c]const c.ra8_fs_mount_t, clus: u32) callconv(.c) u16;
extern fn priv_exfat_next_entry(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.exfat_cursor_t, out: [*c]u8) callconv(.c) u16;
extern fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16;
extern fn priv_exfat_write_dir_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, idx: u32, set: [*c]const u8, bytes: u32) callconv(.c) u16;
extern fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_wr64(p: [*c]u8, v: u64) callconv(.c) void;
extern fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void;

const max_set_bytes: u32 = @intCast(c.k_exfat_max_set_bytes);
const set_writable: u32 = @intCast(c.k_exfat_set_writable);
const entry_bytes: u32 = @intCast(c.k_exfat_entry_bytes);
const off_file_secnt: u32 = @intCast(c.k_exfat_off_file_secnt);
const off_file_csum: u32 = @intCast(c.k_exfat_off_file_csum);
const strm_off_flags: u32 = @intCast(c.k_exfat_strm_off_flags);
const off_strm_valid: u32 = @intCast(c.k_exfat_off_strm_valid);
const strm_off_dlen: u32 = @intCast(c.k_exfat_strm_off_dlen);
const secflag_alloc: u8 = @intCast(c.k_exfat_secflag_alloc);
const secflag_poss: u8 = @intCast(c.k_exfat_secflag_poss);

/// Survey a directory's allocation into run coordinates: a contiguous run is
/// pure arithmetic, a FAT-chained one is walked to its tail. A directory
/// always owns at least one cluster, so there is no owns-nothing case.
pub export fn internal_exfat_dir_survey(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, out_alloc: [*c]u32, out_tail: [*c]u32, out_nofat: [*c]u8) callconv(.c) u16 {
    if (dir[0].contig_end != 0) {
        out_nofat[0] = 1;
        out_alloc[0] = dir[0].contig_end - dir[0].cluster;
        out_tail[0] = dir[0].contig_end - 1;
        return ok;
    }
    out_nofat[0] = 0;
    var clus = dir[0].cluster;
    var held: u32 = 1;
    var guard: u32 = 0;
    while (guard < m[0].count_of_clusters) : (guard += 1) {
        var next: u32 = 0;
        const e = priv_fat_get(m, clus, &next);
        if (e != ok) {
            return e;
        }
        if (priv_is_eoc(m, next) != 0) {
            break;
        }
        if (next < first_data) {
            return c.k_ra8_err_protocol_error;
        }
        clus = next;
        held += 1;
    }
    out_alloc[0] = held;
    out_tail[0] = clus;
    return ok;
}

/// Attach `next` to a directory's run. Staying contiguous writes nothing;
/// a broken run is converted to a real FAT chain first; a chained run just
/// links its tail. `next` is capped with an end-of-chain marker once it is
/// reachable.
pub export fn internal_exfat_dir_link(m: [*c]const c.ra8_fs_mount_t, first: u32, alloc: u32, tail: u32, nofat: [*c]u8, next: u32) callconv(.c) u16 {
    if ((nofat[0] != 0) and (next == tail + 1)) {
        return ok;
    }
    if (nofat[0] != 0) {
        const e = internal_exfat_materialize_run(m, first, alloc, tail, next);
        if (e != ok) {
            return e;
        }
        nofat[0] = 0;
    } else {
        const e = priv_fat_set(m, tail, next);
        if (e != ok) {
            return e;
        }
    }
    return priv_fat_set(m, next, priv_eoc_write(m));
}

/// Rewrite a grown directory's own Stream entry in its parent: read the
/// entry set back from the recorded head, patch the Stream entry's flags,
/// ValidDataLength and DataLength (a directory's valid length equals its
/// data length, and both equal the ALLOCATION), recompute the set checksum
/// and write the set back.
pub export fn internal_exfat_dir_relen(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, new_bytes: u64, nofat: u8) callconv(.c) u16 {
    var set: [max_set_bytes]u8 = @splat(0);
    var cur = std.mem.zeroes(c.exfat_cursor_t);
    cur.cluster = dir[0].self_cluster;
    cur.entry_in_cluster = dir[0].self_index;
    var e = priv_exfat_next_entry(m, &cur, &set);
    if (e != ok) {
        return e;
    }
    const count: u32 = 1 + @as(u32, set[off_file_secnt]);
    if (count > set_writable) {
        return c.k_ra8_err_not_supported;
    }
    var k: u32 = 1;
    while (k < count) : (k += 1) {
        e = priv_exfat_next_entry(m, &cur, &set[k * entry_bytes]);
        if (e != ok) {
            return e;
        }
    }
    const strm = set[entry_bytes..];
    strm[strm_off_flags] = if (nofat != 0) secflag_alloc else secflag_poss;
    priv_wr64(&strm[off_strm_valid], new_bytes);
    priv_wr64(&strm[strm_off_dlen], new_bytes);
    priv_wr16(&set[off_file_csum], priv_exfat_set_checksum(&set, count * entry_bytes));
    return priv_exfat_write_dir_set(m, dir[0].self_cluster, dir[0].self_index, &set, count * entry_bytes);
}

/// The on-disk half of a directory grow: pick a cluster (preferring the
/// tail's successor to stay contiguous), ZERO it before anything links it,
/// mark it used, link it (converting the run to a FAT chain if contiguity
/// broke), and retire the old tail cluster's trailing end-of-directory
/// markers so the walk crosses into the fresh cluster.
pub export fn internal_exfat_dir_append(m: [*c]const c.ra8_fs_mount_t, dir: [*c]const c.exfat_dir_t, alloc: u32, tail: u32, nofat: [*c]u8) callconv(.c) u16 {
    var bmp_lba: u64 = 0;
    var e = priv_exfat_bitmap_lba(m, &bmp_lba);
    if (e != ok) {
        return e;
    }
    var next: u32 = 0;
    e = internal_exfat_pick_cluster(m, bmp_lba, tail + 1, &next);
    if (e != ok) {
        return e;
    }
    e = priv_exfat_zero_cluster(m, next);
    if (e != ok) {
        return e;
    }
    e = priv_exfat_bitmap_mark(m, bmp_lba, next, 1);
    if (e != ok) {
        return e;
    }
    priv_alloc_hint_set(m, next + 1);
    e = internal_exfat_dir_link(m, dir[0].cluster, alloc, tail, nofat, next);
    if (e != ok) {
        return e;
    }
    return priv_exfat_seal_cluster(m, tail);
}

/// Grow a directory by one cluster: survey the run, append a zeroed cluster,
/// then rewrite the directory's own Stream entry -- the volume ROOT has no
/// entry set to patch (its extent is the boot-sector FAT chain the link just
/// extended), a subdirectory must update it or the next mount reads the old
/// length and cannot see the cluster this call added.
pub export fn priv_exfat_grow_dir(m: [*c]const c.ra8_fs_mount_t, dir: [*c]c.exfat_dir_t) callconv(.c) u16 {
    var alloc: u32 = 0;
    var tail: u32 = 0;
    var nofat: u8 = 0;
    var e = internal_exfat_dir_survey(m, dir, &alloc, &tail, &nofat);
    if (e != ok) {
        return e;
    }
    e = internal_exfat_dir_append(m, dir, alloc, tail, &nofat);
    if (e != ok) {
        return e;
    }
    const new_alloc = alloc + 1;
    const cbytes = priv_cluster_bytes(m);
    if (dir[0].self_cluster >= first_data) {
        e = internal_exfat_dir_relen(m, dir, @as(u64, new_alloc) * cbytes, nofat);
        if (e != ok) {
            return e;
        }
    }
    dir[0].contig_end = if (nofat != 0) dir[0].cluster + new_alloc else 0;
    return ok;
}

// ============================================================
// Stream write (RA8FW-1120)
// ============================================================

extern fn priv_write_into_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, off_in_sector: u32, src: [*c]const u8, put: u32) callconv(.c) u16;
extern fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64;
extern fn priv_exfat_file_stamp_write(file_entry: [*c]u8) callconv(.c) void;
extern fn priv_wr32(p: [*c]u8, v: u32) callconv(.c) void;

const strm_off_clus: u32 = @intCast(c.k_exfat_strm_off_clus);
const off_file_attr: u32 = @intCast(c.k_exfat_off_file_attr);
const attr_archive: u8 = @intCast(c.k_exfat_attr_archive);

/// Sector of zeros the gap-filling loop writes from; the Zig side keeps its
/// own read-only copy rather than reaching across to the C one.
const zero_sector: [c.k_ra8_fs_sector_max]u8 = @splat(0);

/// Resolve the cluster at chain index `idx` of an already-grown file. A
/// contiguous file is arithmetic; a chained one walks the FAT from the
/// forward waypoint the read path maintains, so a sequential write costs one
/// FAT lookup per cluster rather than one per cluster from the head.
pub export fn internal_exfat_cluster_at(file: [*c]c.ra8_fs_file_t, idx: u32, out: [*c]u32) callconv(.c) u16 {
    if (file[0].no_fat_chain != 0) {
        out[0] = file[0].first_cluster + idx;
        return ok;
    }
    // Nested rather than joined, because only the inner guard has both
    // answers available: by the time a chained file is walked its waypoint
    // is always a real cluster, so a joined condition would carry a branch
    // no input can make false.
    var from = file[0].first_cluster;
    var hops = idx;
    if (file[0].walk_cache_cluster >= first_data) {
        if (idx >= file[0].walk_cache_idx) {
            from = file[0].walk_cache_cluster;
            hops = idx - file[0].walk_cache_idx;
        }
    }
    var cur = from;
    var i: u32 = 0;
    while (i < hops) : (i += 1) {
        var next: u32 = 0;
        const e = priv_fat_get(file[0].mount, cur, &next);
        if (e != ok) {
            return e;
        }
        if (priv_is_eoc(file[0].mount, next) != 0) {
            return c.k_ra8_err_invalid_state;
        }
        cur = next;
    }
    file[0].walk_cache_idx = idx;
    file[0].walk_cache_cluster = cur;
    out[0] = cur;
    return ok;
}

/// Locate byte position `pos` on the volume, allocating to reach it: the one
/// place both the data loop and the gap-filling loop turn a file offset into
/// a sector plus an offset inside it.
pub export fn internal_exfat_slice_at(file: [*c]c.ra8_fs_file_t, pos: u64, out_lba: [*c]u64, out_off: [*c]u32, out_room: [*c]u32) callconv(.c) u16 {
    const m = file[0].mount;
    const cbytes = priv_cluster_bytes(m);
    const idx: u32 = @intCast(pos / cbytes);
    var e = priv_exfat_ensure_clusters(file, idx + 1);
    if (e != ok) {
        return e;
    }
    var cur: u32 = 0;
    e = internal_exfat_cluster_at(file, idx, &cur);
    if (e != ok) {
        return e;
    }
    file[0].cur_cluster = cur;
    const in_cluster: u32 = @intCast(pos % cbytes);
    const sector = priv_bps(m);
    out_lba[0] = priv_cluster_to_lba(m, cur) + in_cluster / sector;
    out_off[0] = in_cluster % sector;
    out_room[0] = sector - out_off[0];
    return ok;
}

/// Fill `[valid_bytes, offset)` with real zeros: ValidDataLength is a PREFIX
/// length, so once bytes land beyond it the gap cannot be recorded as
/// unwritten -- the only honest answer is to write the zeros a reader is
/// entitled to see. Each iteration advances `valid_bytes` by at least one
/// byte, so the loop is bounded by the gap it was given.
pub export fn internal_exfat_close_gap(file: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    while (file[0].valid_bytes < file[0].offset) {
        var lba: u64 = 0;
        var off: u32 = 0;
        var room: u32 = 0;
        var e = internal_exfat_slice_at(file, file[0].valid_bytes, &lba, &off, &room);
        if (e != ok) {
            return e;
        }
        const gap = file[0].offset - file[0].valid_bytes;
        var n = room;
        if (gap < room) {
            n = @intCast(gap);
        }
        e = priv_write_into_sector(file[0].mount, lba, off, &zero_sector, n);
        if (e != ok) {
            return e;
        }
        file[0].valid_bytes += n;
    }
    return ok;
}

/// Write `len` bytes at the handle's offset: fill the unwritten prefix first,
/// then stream the payload sector by sector, extending the allocation and
/// both lengths as it goes.
pub export fn priv_exfat_write_stream(file: [*c]c.ra8_fs_file_t, buf: [*c]const u8, len: u32) callconv(.c) u16 {
    var e = internal_exfat_close_gap(file);
    if (e != ok) {
        return e;
    }
    var consumed: u32 = 0;
    while (consumed < len) {
        var lba: u64 = 0;
        var off: u32 = 0;
        var room: u32 = 0;
        e = internal_exfat_slice_at(file, file[0].offset, &lba, &off, &room);
        if (e != ok) {
            return e;
        }
        var put = len - consumed;
        if (put > room) {
            put = room;
        }
        e = priv_write_into_sector(file[0].mount, lba, off, buf + consumed, put);
        if (e != ok) {
            return e;
        }
        consumed += put;
        file[0].offset += put;
        if (file[0].offset > file[0].valid_bytes) {
            file[0].valid_bytes = file[0].offset;
        }
        if (file[0].offset > file[0].size_bytes) {
            file[0].size_bytes = file[0].offset;
        }
    }
    return ok;
}

/// Patch a Stream-extension entry to describe the handle's current state:
/// the four fields a stream changes, as the full 64-bit lengths the format
/// defines. NoFatChain is asserted only for a file that actually owns
/// clusters; a contiguous run of nothing is a claim fsck checks.
pub export fn internal_exfat_patch_stream(file: [*c]const c.ra8_fs_file_t, strm: [*c]u8) callconv(.c) void {
    var flags: u8 = secflag_poss;
    var first: u32 = 0;
    if (file[0].alloc_clusters > 0) {
        first = file[0].first_cluster;
        if (file[0].no_fat_chain != 0) {
            flags = secflag_alloc;
        }
    }
    strm[strm_off_flags] = flags;
    priv_wr32(strm + strm_off_clus, first);
    priv_wr64(strm + off_strm_valid, file[0].valid_bytes);
    priv_wr64(strm + strm_off_dlen, file[0].size_bytes);
}

/// Read a file's entry set back into `set`, recording where it lives: the
/// ordinary directory cursor gathers it, so a set that straddles a cluster
/// boundary is read correctly rather than by flat arithmetic that would run
/// off the end of the first cluster.
pub export fn internal_exfat_gather_set(file: [*c]const c.ra8_fs_file_t, set: [*c]u8, at_file: [*c]c.exfat_setpos_t, at_stream: [*c]c.exfat_setpos_t) callconv(.c) u16 {
    var cur = c.exfat_cursor_t{ .cluster = file[0].entry_set_cluster, .entry_in_cluster = file[0].entry_set_index, .scanned = 0, .contig_end = 0 };
    var k: u32 = 0;
    while (k < file[0].entry_set_count) : (k += 1) {
        const here = c.exfat_setpos_t{ .cluster = cur.cluster, .index = cur.entry_in_cluster };
        const e = priv_exfat_next_entry(file[0].mount, &cur, set + (k * entry_bytes));
        if (e != ok) {
            return e;
        }
        if (k == 0) {
            at_file[0] = here;
        }
        if (k == 1) {
            at_stream[0] = here;
        }
    }
    return ok;
}

/// Flush a file's entry set: stamp, patch the Stream entry, recompute the
/// set checksum, and write the File and Stream entries back at the
/// coordinates the gather recorded. Stamp and patch happen BEFORE the
/// checksum: it covers every byte both touch, so the other order writes a
/// set a host fsck rejects.
pub export fn priv_exfat_flush_set(file: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    const m = file[0].mount;
    var set: [max_set_bytes]u8 = @splat(0);
    var at_file = c.exfat_setpos_t{ .cluster = 0, .index = 0 };
    var at_stream = c.exfat_setpos_t{ .cluster = 0, .index = 0 };
    var e = internal_exfat_gather_set(file, &set, &at_file, &at_stream);
    if (e != ok) {
        return e;
    }
    priv_exfat_file_stamp_write(&set);
    // A write also sets the archive attribute: the convention a backup tool
    // clears and the OS re-sets on modification. The low byte of the 16-bit
    // FileAttributes field carries it; the other bits are preserved.
    set[off_file_attr] |= attr_archive;
    internal_exfat_patch_stream(file, &set[entry_bytes]);
    priv_wr16(&set[off_file_csum], priv_exfat_set_checksum(&set, file[0].entry_set_count * entry_bytes));
    e = priv_exfat_write_dir_set(m, at_file.cluster, at_file.index, &set, entry_bytes);
    if (e != ok) {
        return e;
    }
    return priv_exfat_write_dir_set(m, at_stream.cluster, at_stream.index, &set[entry_bytes], entry_bytes);
}
