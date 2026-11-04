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

extern fn priv_alloc_eoc_cluster(m: [*c]const c.ra8_fs_mount_t, out_c: [*c]u32) callconv(.c) u16;
extern fn priv_fat_set(m: [*c]const c.ra8_fs_mount_t, cluster: u32, value: u32) callconv(.c) u16;
extern fn priv_eoc_write(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_free_chain(m: [*c]const c.ra8_fs_mount_t, start: u32) callconv(.c) u16;

/// Free the cluster tail past `new_size` on a FAT file: below the new
/// cluster count the chain is left alone; at and above it the survivor's
/// tail is re-capped with an end-of-chain marker and the rest is freed.
/// Shrinking to zero drops the whole chain and clears the first cluster.
pub export fn internal_fat_trunc_shrink(file: [*c]c.ra8_fs_file_t, new_size: u32, cbytes: u32) callconv(.c) u16 {
    const m: [*c]const c.ra8_fs_mount_t = file[0].mount;
    const new_nclus: u32 = @intCast(internal_trunc_clusters(new_size, cbytes));
    if (new_nclus == 0) {
        if (file[0].first_cluster >= c.k_cluster_first_data) {
            const e = priv_free_chain(m, file[0].first_cluster);
            if (e != ok) {
                return e;
            }
        }
        file[0].first_cluster = 0;
        file[0].size_bytes = new_size;
        return ok;
    }
    var new_tail: u32 = 0;
    const we = internal_trunc_walk(m, file[0].first_cluster, new_nclus - 1, &new_tail);
    if (we != ok) {
        return we;
    }
    var succ: u32 = 0;
    const ge = priv_fat_get(m, new_tail, &succ);
    if (ge != ok) {
        return ge;
    }
    if (priv_is_eoc(m, succ) == 0) {
        var e = priv_fat_set(m, new_tail, priv_eoc_write(m));
        if (e != ok) {
            return e;
        }
        e = priv_free_chain(m, succ);
        if (e != ok) {
            return e;
        }
    }
    file[0].size_bytes = new_size;
    return ok;
}

/// Append zeroed clusters to a FAT file until it owns `new_nclus`:
/// allocates the first cluster when the file has none, then links a fresh
/// end-of-chain cluster on at a time, zeroing each as it is created. One
/// forward pass, linear in the clusters added.
pub export fn internal_fat_trunc_extend(file: [*c]c.ra8_fs_file_t, tail: u32, have: u32, new_nclus: u32, cbytes: u32) callconv(.c) u16 {
    const m: [*c]const c.ra8_fs_mount_t = file[0].mount;
    var cur = tail;
    var owned = have;
    if (owned == 0) {
        var e = priv_alloc_eoc_cluster(m, &cur);
        if (e != ok) {
            return e;
        }
        file[0].first_cluster = cur;
        e = internal_trunc_zero_span(m, cur, 0, cbytes);
        if (e != ok) {
            return e;
        }
        owned = 1;
    }
    while (owned < new_nclus) {
        var cl: u32 = 0;
        var e = priv_alloc_eoc_cluster(m, &cl);
        if (e != ok) {
            return e;
        }
        e = priv_fat_set(m, cur, cl);
        if (e != ok) {
            return e;
        }
        e = internal_trunc_zero_span(m, cl, 0, cbytes);
        if (e != ok) {
            return e;
        }
        cur = cl;
        owned += 1;
    }
    return ok;
}

/// Grow a FAT file to `new_size`, zero-filling `[old_size, new_size)`:
/// the slack of the last old cluster is cleared first (keeping the real
/// data below it), then the chain is extended from that same cluster so it
/// is walked only once.
pub export fn internal_fat_trunc_grow(file: [*c]c.ra8_fs_file_t, new_size: u32, cbytes: u32) callconv(.c) u16 {
    const m: [*c]const c.ra8_fs_mount_t = file[0].mount;
    const old_size: u32 = @intCast(file[0].size_bytes);
    const old_nclus: u32 = @intCast(internal_trunc_clusters(old_size, cbytes));
    const new_nclus: u32 = @intCast(internal_trunc_clusters(new_size, cbytes));
    var tail: u32 = 0;
    if (old_nclus > 0) {
        const we = internal_trunc_walk(m, file[0].first_cluster, old_nclus - 1, &tail);
        if (we != ok) {
            return we;
        }
        const base = (old_nclus - 1) * cbytes;
        const cap = old_nclus * cbytes;
        const span_hi = if (new_size < cap) new_size else cap;
        const ze = internal_trunc_zero_span(m, tail, old_size - base, span_hi - base);
        if (ze != ok) {
            return ze;
        }
    }
    const ee = internal_fat_trunc_extend(file, tail, old_nclus, new_nclus, cbytes);
    if (ee != ok) {
        return ee;
    }
    file[0].size_bytes = new_size;
    return ok;
}
