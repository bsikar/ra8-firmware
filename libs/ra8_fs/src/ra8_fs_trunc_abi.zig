//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The truncate helpers shared by the FAT and exFAT truncate paths: the
//! cluster count a byte length needs, the chain walker that reaches a
//! cluster by index, and the sector-wise read-modify-write zeroing of a
//! byte span inside one cluster. Ported from the slice of
//! `ra8_fs_fat_truncate.c` (RA8FW-1115); the C file keeps the truncate
//! bodies.

const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_protocol: u16 = c.k_ra8_err_protocol_error;

extern fn priv_fat_get(m: [*c]const c.ra8_fs_mount_t, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_is_eoc(m: [*c]const c.ra8_fs_mount_t, value: u32) callconv(.c) u8;
extern fn priv_cluster_to_lba(m: [*c]const c.ra8_fs_mount_t, cluster: u32) callconv(.c) u64;
extern fn priv_bps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_sec_io() callconv(.c) [*c]u8;
extern fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]const u8) callconv(.c) u16;

/// Clusters a byte length needs: ceiling division by the cluster size.
pub export fn internal_trunc_clusters(bytes: u64, cbytes: u32) callconv(.c) u64 {
    return (bytes + cbytes - 1) / cbytes;
}

/// Walk `idx` clusters forward along a chain from `start`; a chain that
/// ends before `idx` is a corrupt file, not a short one, so it is reported
/// rather than clamped.
pub export fn internal_trunc_walk(m: [*c]const c.ra8_fs_mount_t, start: u32, idx: u32, out: [*c]u32) callconv(.c) u16 {
    var cur = start;
    var i: u32 = 0;
    while (i < idx) : (i += 1) {
        var next: u32 = 0;
        const e = priv_fat_get(m, cur, &next);
        if (e != ok) {
            return e;
        }
        if (priv_is_eoc(m, next) != 0) {
            return err_protocol;
        }
        cur = next;
    }
    out.* = cur;
    return ok;
}

/// Write real zeros over the byte range `[lo, hi)` inside one cluster,
/// sector by sector read-modify-write, so the neighbours on the two edge
/// sectors survive. `lo >= hi` is nothing to do.
pub export fn internal_trunc_zero_span(m: [*c]const c.ra8_fs_mount_t, cluster: u32, lo: u32, hi: u32) callconv(.c) u16 {
    if (lo >= hi) {
        return ok;
    }
    const base_lba: u64 = priv_cluster_to_lba(m, cluster);
    const bps: u32 = priv_bps(m);
    var off = lo;
    while (off < hi) {
        const sec_idx = off / bps;
        const in_sector = off - sec_idx * bps;
        var n = bps - in_sector;
        if (n > hi - off) {
            n = hi - off;
        }
        const sec = priv_sec_io();
        const re = priv_read_sector(m, base_lba + sec_idx, sec);
        if (re != ok) {
            return re;
        }
        var j: u32 = 0;
        while (j < n) : (j += 1) {
            sec[@intCast(in_sector + j)] = 0;
        }
        const we = priv_write_sector(m, base_lba + sec_idx, sec);
        if (we != ok) {
            return we;
        }
        off += n;
    }
    return ok;
}
