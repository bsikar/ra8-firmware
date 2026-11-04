//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The directory walker: initializes a `dir_walk_t` over a `dir_loc_t` and
//! advances it one sector at a time, over the FAT12/16 fixed root region and
//! FAT32 cluster chains (a subdirectory is that same chain case starting at
//! an arbitrary cluster). Ported from the slice of `ra8_fs_fat_name.c`
//! (RA8FW-1113); the C file keeps the name-lookup bodies.

const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;

extern fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64;
extern fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8;
extern fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;

/// Point the walk at the volume root: a FAT32 root is a cluster chain from
/// `root_cluster`; the FAT12/16 root lives in the fixed region at
/// `first_root_lba` sized `root_entries` entries.
pub export fn internal_dir_walk_init_root(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t) callconv(.c) void {
    if (m[0].type == c.k_ra8_fs_type_fat32) {
        w[0].is_root_fixed = 0;
        w[0].fixed_remaining = 0;
        w[0].cluster = m[0].root_cluster;
        w[0].sector_in_cluster = 0;
        w[0].cur_lba = priv_cluster_to_lba(m, w[0].cluster);
    } else {
        const bps: u32 = priv_bps(m);
        const root_dir_sectors = (@as(u32, m[0].root_entries) * entry_bytes + (bps - 1)) / bps;
        w[0].is_root_fixed = 1;
        w[0].fixed_remaining = root_dir_sectors;
        w[0].cluster = 0;
        w[0].sector_in_cluster = 0;
        w[0].cur_lba = m[0].first_root_lba;
    }
    w[0].entry_idx = 0;
    w[0].cluster_hops = 0;
}

/// Point the walk at the directory `loc` names: the volume root, or the
/// subdirectory whose first cluster is `loc.cluster`.
pub export fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void {
    if (loc[0].is_root != 0) {
        internal_dir_walk_init_root(m, w);
        return;
    }
    w[0].is_root_fixed = 0;
    w[0].fixed_remaining = 0;
    w[0].cluster = loc[0].cluster;
    w[0].sector_in_cluster = 0;
    w[0].cur_lba = priv_cluster_to_lba(m, loc[0].cluster);
    w[0].entry_idx = 0;
    w[0].cluster_hops = 0;
}

/// Advance the walk one sector. A fixed root reports the end of directory
/// once the region's last sector is behind the cursor; a cluster chain moves
/// through the sectors inside the cluster, then follows the FAT when the
/// cluster is consumed, reporting the end when the chain reaches EOC and a
/// protocol error when it cycles past `count_of_clusters` hops.
pub export fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16 {
    out_eod[0] = 0;
    if (w[0].is_root_fixed != 0) {
        if (w[0].fixed_remaining <= 1) {
            out_eod[0] = 1;
            return ok;
        }
        w[0].fixed_remaining -= 1;
        w[0].cur_lba += 1;
        w[0].entry_idx = 0;
        return ok;
    }
    w[0].sector_in_cluster += 1;
    if (w[0].sector_in_cluster >= @as(u32, m[0].sectors_per_cluster)) {
        var next: u32 = 0;
        const err = priv_fat_get(m, w[0].cluster, &next);
        if (err != ok) return err;
        if (priv_is_eoc(m, next) != 0) {
            out_eod[0] = 1;
            return ok;
        }
        // Cycle guard (NASA Rule 2): a healthy chain visits at most
        // count_of_clusters distinct clusters; more means a corrupt loop.
        w[0].cluster_hops += 1;
        if (w[0].cluster_hops > @as(u32, m[0].count_of_clusters)) {
            return err_protocol;
        }
        w[0].cluster = next;
        w[0].sector_in_cluster = 0;
        w[0].cur_lba = priv_cluster_to_lba(m, w[0].cluster);
    } else {
        w[0].cur_lba += 1;
    }
    w[0].entry_idx = 0;
    return ok;
}
