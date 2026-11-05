//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT cluster growth (RA8FW-1118): pick the next cluster, turn a
//! contiguous run into a real FAT chain, link a freshly allocated cluster
//! onto a file's allocation, and grow one cluster at a time. Ported from the
//! allocation half of `ra8_fs_fat_exfat_stream.c`; the directory-growth and
//! stream-write halves stay in C and call back into the exports here.

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
