//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT growth (RA8FW-1118 cluster growth, RA8FW-1119 directory growth):
//! pick the next cluster, turn a contiguous run into a real FAT chain, link
//! a freshly allocated cluster onto a file's or directory's allocation, grow
//! one cluster at a time, and rewrite a grown directory's own Stream entry.
//! Ported from `ra8_fs_fat_exfat_stream.c`; the stream-write half stays in C.

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
