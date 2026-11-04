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

extern fn priv_sec_walk() callconv(.c) [*c]u8;
extern fn priv_entry_set_cluster_size(entry: [*c]u8, cluster: u32, size: u32) callconv(.c) void;
extern fn priv_fat_entry_stamp_write(entry: [*c]u8) callconv(.c) void;
extern fn priv_cluster_bytes(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;

/// Rewrite a FAT file's directory entry after its length changed: patches
/// the entry's first cluster and size, stamps the write time (a truncate is
/// a content change even when no byte is written), and marks the handle
/// dirty so close flushes FSInfo.
pub export fn internal_fat_trunc_commit(file: [*c]c.ra8_fs_file_t) callconv(.c) u16 {
    const m: [*c]const c.ra8_fs_mount_t = file[0].mount;
    const sec = priv_sec_walk();
    const re = priv_read_sector(m, file[0].dir_entry_lba, sec);
    if (re != ok) {
        return re;
    }
    priv_entry_set_cluster_size(&sec[file[0].dir_entry_idx], file[0].first_cluster, @intCast(file[0].size_bytes));
    priv_fat_entry_stamp_write(&sec[file[0].dir_entry_idx]);
    const we = priv_write_sector(m, file[0].dir_entry_lba, sec);
    if (we != ok) {
        return we;
    }
    file[0].dirty = 1;
    return ok;
}

/// Truncate a FAT file to `new_size` and commit its directory entry:
/// dispatches to the shrink or grow branch, resets the read accelerator
/// (a shrink can free the cluster it cached, a grow moves the tail), and
/// rewrites the directory entry once.
pub export fn internal_fat_trunc(file: [*c]c.ra8_fs_file_t, new_size: u32) callconv(.c) u16 {
    const cbytes = priv_cluster_bytes(file[0].mount);
    var e: u16 = ok;
    if (new_size < file[0].size_bytes) {
        e = internal_fat_trunc_shrink(file, new_size, cbytes);
    } else if (new_size > file[0].size_bytes) {
        e = internal_fat_trunc_grow(file, new_size, cbytes);
    }
    if (e != ok) {
        return e;
    }
    file[0].cur_cluster = file[0].first_cluster;
    file[0].walk_cache_idx = 0;
    file[0].walk_cache_cluster = file[0].first_cluster;
    return internal_fat_trunc_commit(file);
}

extern fn priv_wr32(p: [*c]u8, v: u32) callconv(.c) void;
extern fn priv_wr64(p: [*c]u8, v: u64) callconv(.c) void;
extern fn priv_exfat_free_clusters(m: [*c]const c.ra8_fs_mount_t, strm: [*c]const u8) callconv(.c) u16;

/// Release a cluster run by describing it as a Stream-extension entry:
/// `priv_exfat_free_clusters` reads first cluster, byte length and the
/// NoFatChain flag out of a 32-byte argument that never exists on disk.
pub export fn internal_exfat_free_run(m: [*c]const c.ra8_fs_mount_t, first: u32, nbytes: u64, nofat: u8) callconv(.c) u16 {
    var strm: [c.k_exfat_entry_bytes]u8 = undefined;
    @memset(&strm, 0);
    priv_wr32(&strm[@as(usize, @intCast(c.k_exfat_strm_off_clus))], first);
    priv_wr64(&strm[@as(usize, @intCast(c.k_exfat_strm_off_dlen))], nbytes);
    strm[@as(usize, @intCast(c.k_exfat_strm_off_flags))] =
        if (nofat != 0) @as(u8, @intCast(c.k_exfat_secflag_alloc)) else @as(u8, @intCast(c.k_exfat_secflag_poss));
    return priv_exfat_free_clusters(m, &strm);
}

/// Trim a FAT-chained exFAT file to `new_nclus` clusters: walk to the
/// survivor's new tail, cap it with an end-of-chain marker, and free the
/// remainder off the allocation bitmap from the tail's old successor. The
/// freed clusters keep their stale FAT links; the bitmap is exFAT's
/// authority and nothing follows a free cluster's chain.
pub export fn internal_exfat_shrink_chain_tail(file: [*c]c.ra8_fs_file_t, new_nclus: u32, cbytes: u32) callconv(.c) u16 {
    const m = file[0].mount;
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
    var e = priv_fat_set(m, new_tail, priv_eoc_write(m));
    if (e != ok) {
        return e;
    }
    e = internal_exfat_free_run(m, succ, cbytes, 0);
    if (e != ok) {
        return e;
    }
    file[0].tail_cluster = new_tail;
    return ok;
}

/// Free an exFAT file's cluster tail past `new_size`: a contiguous run
/// keeps `[first, first + new_nclus)` and frees the rest by count; a
/// FAT-chained run re-terminates the survivor via the chained arm; zero
/// drops the whole allocation and returns the handle to the empty shape a
/// fresh open produces.
pub export fn internal_exfat_trunc_shrink(file: [*c]c.ra8_fs_file_t, new_size: u64, cbytes: u32) callconv(.c) u16 {
    const m = file[0].mount;
    const new_nclus: u32 = @intCast(internal_trunc_clusters(new_size, cbytes));
    if (new_nclus < file[0].alloc_clusters) {
        if (new_nclus == 0) {
            const e0 = internal_exfat_free_run(m, file[0].first_cluster, file[0].size_bytes, file[0].no_fat_chain);
            if (e0 != ok) {
                return e0;
            }
            file[0].first_cluster = 0;
            file[0].no_fat_chain = 1;
            file[0].tail_cluster = 0;
        } else if (file[0].no_fat_chain != 0) {
            const first = file[0].first_cluster + new_nclus;
            const e1 = internal_exfat_free_run(m, first, @as(u64, file[0].alloc_clusters - new_nclus) * cbytes, 1);
            if (e1 != ok) {
                return e1;
            }
            file[0].tail_cluster = file[0].first_cluster + new_nclus - 1;
        } else {
            const e2 = internal_exfat_shrink_chain_tail(file, new_nclus, cbytes);
            if (e2 != ok) {
                return e2;
            }
        }
        file[0].alloc_clusters = new_nclus;
    }
    file[0].size_bytes = new_size;
    if (file[0].valid_bytes > new_size) {
        file[0].valid_bytes = new_size;
    }
    return ok;
}

extern fn priv_exfat_ensure_clusters(file: [*c]c.ra8_fs_file_t, need: u32) callconv(.c) u16;
extern fn priv_exfat_flush_set(file: [*c]c.ra8_fs_file_t) callconv(.c) u16;
extern fn priv_lock_acquire() callconv(.c) void;
extern fn priv_lock_release() callconv(.c) void;

const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_invalid_size: u16 = c.k_ra8_err_invalid_size;

/// Truncate an exFAT file to `new_size` and flush its entry set: shrink
/// frees the tail; grow takes clusters from the allocation bitmap and
/// raises DataLength alone, so ValidDataLength stays at the written
/// prefix and the read path serves the grown gap as zero without a byte
/// being written for it (exFAT spec sec 7.4.5). Either way the entry set
/// is rewritten so a later mount sees the new length.
pub export fn internal_exfat_trunc(file: [*c]c.ra8_fs_file_t, new_size: u64) callconv(.c) u16 {
    const cbytes = priv_cluster_bytes(file[0].mount);
    if (new_size < file[0].size_bytes) {
        const e = internal_exfat_trunc_shrink(file, new_size, cbytes);
        if (e != ok) {
            return e;
        }
    } else if (new_size > file[0].size_bytes) {
        // Sized before a single cluster moves: a grow past the volume's
        // whole cluster count can never succeed, and discovering that
        // AFTER the bitmap ran dry would leave the volume full for no gain.
        const want = internal_trunc_clusters(new_size, cbytes);
        if (want > @as(u64, file[0].mount[0].count_of_clusters)) {
            return err_no_mem;
        }
        const e = priv_exfat_ensure_clusters(file, @intCast(want));
        if (e != ok) {
            return e;
        }
        file[0].size_bytes = new_size;
    } else {
        return ok;
    }
    return priv_exfat_flush_set(file);
}

/// Truncate the file behind `file` -- the guarded body of `ra8_fs_truncate`:
/// validates the handle, dispatches on the mount's filesystem, and pulls the
/// offset down if a shrink left it past the new end (the offset is otherwise
/// left where the caller had it, as `ftruncate()` promises). The mode/handle
/// guard is one compound decision of two conditions -- a closed handle, or a
/// handle opened read-only -- and its vectors live with the test that drives
/// it, cited as `libs/ra8_fs/src/ra8_fs_trunc_abi.zig@internal_truncate_locked`.
pub export fn internal_truncate_locked(file: [*c]c.ra8_fs_file_t, new_size: u64) callconv(.c) u16 {
    if (file == null) {
        return err_null_ptr;
    }
    if (file[0].in_use == 0 or file[0].mode == c.k_ra8_fs_mode_read) {
        return err_invalid_state;
    }
    var e: u16 = ok;
    if (file[0].mount[0].type == c.k_ra8_fs_type_exfat) {
        e = internal_exfat_trunc(file, new_size);
    } else if (new_size > @as(u64, @intCast(c.k_ra8_fs_fat_max_file_bytes))) {
        // The FAT boundary of the 4 GiB cap: `DIR_FileSize` cannot record
        // 4 GiB or more, so the request is refused rather than wrapped.
        e = err_invalid_size;
    } else {
        e = internal_fat_trunc(file, @intCast(new_size));
    }
    if (e != ok) {
        return e;
    }
    if (file[0].offset > file[0].size_bytes) {
        file[0].offset = file[0].size_bytes;
    }
    return ok;
}

/// Public truncate entry: takes the library lock, runs the guarded body,
/// releases it. Lock contract (RA8_OWNS_RESOURCE in the C original): this
/// function owns `ra8_fs_lock` for the duration of the call.
pub export fn ra8_fs_truncate(file: [*c]c.ra8_fs_file_t, new_size: u64) callconv(.c) u16 {
    priv_lock_acquire();
    const err = internal_truncate_locked(file, new_size);
    priv_lock_release();
    return err;
}
