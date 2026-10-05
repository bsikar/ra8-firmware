//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_fs_free_space`: report a mounted volume's capacity, used and free
//! space in clusters and bytes.
//!
//! FAT has no reliable on-disk free count, so the first query scans every
//! FAT entry and caches the result on the mount (allocation and free keep it
//! current after that). exFAT counts the set bits of its allocation bitmap,
//! masking the tail byte so no bits past the last cluster are counted.
//! The FAT reader, bitmap locator, free-count cache and sector buffers stay in
//! C, reached through `fs_c.zig`.
//!
//! Bounded loops: the FAT scan runs `count_of_clusters` times, the bitmap
//! walk `ceil(count_of_clusters / 8)` times.

const c = @import("fs_c.zig").c;
const lock = @import("ra8_fs_lock_abi.zig");

pub const Mount = c.ra8_fs_mount_t;
pub const Space = c.ra8_fs_space_t;

pub const ok: u16 = c.k_ra8_ok;
pub const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
pub const err_invalid_state: u16 = c.k_ra8_err_invalid_state;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const cluster_free: u32 = @intCast(c.k_cluster_free);
const free_unknown: u32 = @intCast(c.k_fs_free_unknown);

/// Free clusters from a full FAT scan.
fn fatFree(m: *const Mount, out: *u32) u16 {
    var free: u32 = 0;
    var clus: u32 = first_data;
    const last = first_data + m.count_of_clusters;
    while (clus < last) : (clus += 1) {
        var v: u32 = 0;
        const err = c.priv_fat_get(m, clus, &v);
        if (err != ok) return err;
        if (v == cluster_free) free += 1;
    }
    out.* = free;
    return ok;
}

/// Free clusters from the exFAT allocation bitmap.
fn exfatFree(m: *const Mount, out: *u32) u16 {
    var bmp_clus: u32 = 0;
    var bmp_len: u32 = 0;
    const ferr = c.priv_exfat_find_bitmap(m, &bmp_clus, &bmp_len);
    if (ferr != ok) return ferr;
    const bmp_lba = c.priv_cluster_to_lba(m, bmp_clus);
    const total = m.count_of_clusters;
    const full_bytes = total >> 3;
    const rem_bits: u3 = @intCast(total & 7);
    const nbytes = full_bytes + @intFromBool(rem_bits != 0);
    const bps = c.priv_bps(m);
    const sec = c.priv_sec_io();
    var loaded: u64 = ~@as(u64, 0);
    var used: u32 = 0;
    var bi: u32 = 0;
    while (bi < nbytes) : (bi += 1) {
        const lba = bmp_lba + bi / bps;
        if (lba != loaded) {
            const err = c.priv_read_sector(m, lba, sec);
            if (err != ok) return err;
            loaded = lba;
        }
        var b: u8 = sec[bi % bps];
        if (bi == full_bytes) b &= (@as(u8, 1) << rem_bits) - 1;
        used += @popCount(b);
    }
    out.* = total - used;
    return ok;
}

fn freeClusters(m: *const Mount, out: *u32) u16 {
    if (m.type == c.k_ra8_fs_type_exfat) return exfatFree(m, out);
    const cached = c.priv_free_count_peek(m);
    if (cached != free_unknown) {
        out.* = cached;
        return ok;
    }
    const err = fatFree(m, out);
    if (err != ok) return err;
    c.priv_free_count_cache(m, out.*);
    return ok;
}

fn spaceLocked(handle: ?*const Mount, dst: ?*Space) u16 {
    const m = handle orelse return err_null_ptr;
    const out = dst orelse return err_null_ptr;
    if (m.in_use == 0) return err_invalid_state;
    var free: u32 = 0;
    const err = freeClusters(m, &free);
    if (err != ok) return err;
    const total = m.count_of_clusters;
    const bpc = c.priv_cluster_bytes(m);
    const used = total - free;
    out.bytes_per_cluster = bpc;
    out.total_clusters = total;
    out.free_clusters = free;
    out.used_clusters = used;
    out.total_bytes = @as(u64, total) * bpc;
    out.free_bytes = @as(u64, free) * bpc;
    out.used_bytes = @as(u64, used) * bpc;
    return ok;
}

pub export fn ra8_fs_free_space(handle: ?*const Mount, out: ?*Space) callconv(.C) u16 {
    lock.priv_lock_acquire();
    defer lock.priv_lock_release();
    return spaceLocked(handle, out);
}
