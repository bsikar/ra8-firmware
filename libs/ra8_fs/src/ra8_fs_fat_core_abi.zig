//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! FAT value primitives and the cluster allocator: the little-endian 32/64-bit
//! field writers, the byte-compare, the end-of-chain write selector, and the
//! free-cluster scan behind `priv_alloc_cluster()`.
//!
//! The FAT entry get/set layer and its FAT12 straddle helpers moved to the
//! entry module (RA8FW-1098), and the readers, the 16-bit writer, the byte
//! fill/copy, and the sector backends live in the primitive module (RA8FW-1097).
//!
//! The allocator scans at most `count_of_clusters` entries starting from the
//! allocation hint, wrapping once, and takes the first free entry. No
//! allocation: state lives in the mount and the allocator-state module.

const std = @import("std");
const c = @import("fs_c.zig").c;

pub const Mount = c.ra8_fs_mount_t;

const ok: u16 = c.k_ra8_ok;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const first_data: u32 = c.k_cluster_first_data;
const cluster_free: u32 = c.k_cluster_free;

extern fn priv_fat_get(m: [*c]const Mount, cluster: u32, out_value: [*c]u32) callconv(.c) u16;
extern fn priv_alloc_hint_get(m: [*c]const Mount) callconv(.c) u32;
extern fn priv_alloc_hint_set(m: [*c]const Mount, cluster: u32) callconv(.c) void;
extern fn priv_free_count_took(m: [*c]const Mount, n: u32) callconv(.c) void;

/// Write a little-endian 32-bit field.
pub export fn priv_wr32(p: [*c]u8, v: u32) callconv(.c) void {
    p[0] = @truncate(v);
    p[1] = @truncate(v >> 8);
    p[2] = @truncate(v >> 16);
    p[3] = @truncate(v >> 24);
}

/// Write a little-endian 64-bit field.
pub export fn priv_wr64(p: [*c]u8, v: u64) callconv(.c) void {
    priv_wr32(p, @truncate(v));
    priv_wr32(p + 4, @truncate(v >> 32));
}

/// Byte compare: 1 when the first `n` bytes match, 0 otherwise.
pub export fn priv_byte_equal(a: [*c]const u8, b: [*c]const u8, n: u32) callconv(.c) u8 {
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/// The value written to terminate a cluster chain on this FAT type.
pub export fn priv_eoc_write(m: [*c]const Mount) callconv(.c) u32 {
    if (m[0].type == c.k_ra8_fs_type_fat12) return c.k_cluster_eoc_write_fat12;
    if (m[0].type == c.k_ra8_fs_type_fat16) return c.k_cluster_eoc_write_fat16;
    if (m[0].type == c.k_ra8_fs_type_exfat) return c.k_cluster_eoc_write_exfat;
    return c.k_cluster_eoc_write_fat32;
}

/// Clamp the next-free hint into the data clusters: anything at or past the
/// end of the volume restarts the scan at the first data cluster. A hint
/// below 2 wraps in unsigned arithmetic and fails the same test, so no
/// second condition is needed.
fn allocStart(m: [*c]const Mount, hint: u32) u32 {
    if (hint -% first_data >= m[0].count_of_clusters) return first_data;
    return hint;
}

/// Allocate one free cluster: scan from the hint, wrapping once over the
/// whole data region, and take the first free entry.
pub export fn priv_alloc_cluster(m: [*c]const Mount, out_cluster: [*c]u32) callconv(.c) u16 {
    const past_end: u32 = first_data + m[0].count_of_clusters;
    var cur: u32 = allocStart(m, priv_alloc_hint_get(m));
    var seen: u32 = 0;
    while (seen < m[0].count_of_clusters) : (seen += 1) {
        var v: u32 = 0;
        const err = priv_fat_get(m, cur, &v);
        if (err != ok) return err;
        if (v == cluster_free) {
            out_cluster.* = cur;
            priv_alloc_hint_set(m, cur + 1);
            priv_free_count_took(m, 1);
            return ok;
        }
        cur += 1;
        if (cur >= past_end) cur = first_data;
    }
    return err_no_mem;
}
