//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Directory write-path helpers for FAT: the port of the static slice of
//! `ra8_fs_fat_lfn_write.c` that derives a unique 8.3 alias for a long
//! name and advances a reserved-slot cursor across a directory.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;
const alias_tail_max: u32 = @intCast(c.k_lfn_alias_tail_max);

extern fn priv_lfn_alias_basis(leaf: [*c]const u16, n: u32, tail: u32, out11: [*c]u8) callconv(.c) void;
extern fn priv_dir_find(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, name83: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16;
extern fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16;

/// Probe alias bases in tail order until one finds no directory entry,
/// bounded by `k_lfn_alias_tail_max` (NASA Power of 10 Rule 2). Exhausting
/// the bound needs a million aliases on one basis name; the bound's honest
/// failure answer is `no_mem`, not a hang.
pub export fn internal_alias_unique(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u16, n: u32, out11: [*c]u8) callconv(.c) u16 {
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [entry_bytes]u8 = @splat(0);
    var tail: u32 = 1;
    while (tail <= alias_tail_max) : (tail += 1) {
        priv_lfn_alias_basis(leaf, n, tail, out11);
        const err = priv_dir_find(m, loc, out11, &lba, &off, &entry);
        if (err == err_not_found) {
            return ok;
        }
        if (err != ok) {
            return err;
        }
    }
    return err_no_mem;
}

/// Advance a reserved-slot cursor to the next slot, rolling over through
/// `priv_dir_walk_next_sector` at the sector's end: the finished sector is
/// written back before the walk moves on, and the new sector is read in.
/// The walk running off the directory answers `no_mem`.
pub export fn internal_slot_advance(m: [*c]const c.ra8_fs_mount_t, cur: [*c]c.dir_slot_t, buf: [*c]u8, lba_io: [*c]u64) callconv(.c) u16 {
    cur.*.ent += 1;
    if (cur.*.ent < priv_dir_eps(m)) {
        return ok;
    }
    var err = priv_write_sector(m, lba_io.*, buf);
    if (err != ok) {
        return err;
    }
    var eod: u8 = 0;
    err = priv_dir_walk_next_sector(m, &cur.*.w, &eod);
    if (err != ok) {
        return err;
    }
    if (eod != 0) {
        return err_no_mem;
    }
    cur.*.ent = 0;
    lba_io.* = cur.*.w.cur_lba;
    return priv_read_sector(m, lba_io.*, buf);
}

extern fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void;
extern fn priv_alloc_eoc_cluster(m: [*c]const c.ra8_fs_mount_t, out_c: [*c]u32) callconv(.c) u16;
extern fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64;
extern fn priv_free_chain(m: [*c]const c.ra8_fs_mount_t, start: u32) callconv(.c) u16;
extern fn priv_fat_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, value: u32) callconv(.c) u16;

var zero_sector: [512]u8 = @splat(0);

/// Append one zeroed cluster to a directory's chain: walk to the chain's
/// last cluster with the shared directory iterator (its cycle guard comes
/// along), allocate a fresh cluster, zero every sector of it so it reads as
/// end-of-directory, and only then link it in. A FAT12/16 volume root has no
/// chain to extend and is refused. On failure the fresh cluster is freed,
/// so nothing is left allocated but unreferenced.
pub export fn internal_dir_grow(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t) callconv(.c) u16 {
    if (loc.*.is_root != 0 and m.*.type != c.k_ra8_fs_type_fat32) {
        return err_no_mem;
    }
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    priv_dir_walk_init_loc(m, loc, &w);
    var last: u32 = w.cluster;
    var eod: u8 = 0;
    while (eod == 0) {
        last = w.cluster;
        const werr = priv_dir_walk_next_sector(m, &w, &eod);
        if (werr != ok) {
            return werr;
        }
    }
    var fresh: u32 = 0;
    var err = priv_alloc_eoc_cluster(m, &fresh);
    if (err != ok) {
        return err;
    }
    const base = priv_cluster_to_lba(m, fresh);
    var s: u32 = 0;
    while (s < m.*.sectors_per_cluster) : (s += 1) {
        err = priv_write_sector(m, base + s, &zero_sector);
        if (err != ok) {
            _ = priv_free_chain(m, fresh);
            return err;
        }
    }
    err = priv_fat_set(m, last, fresh);
    if (err != ok) {
        _ = priv_free_chain(m, fresh);
        return err;
    }
    return ok;
}
