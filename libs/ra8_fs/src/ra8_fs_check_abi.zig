//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared scaffolding for the read-only consistency check (fsck): the finding
//! recorder, the visited-cluster bitmap primitives, and the walk-stop helper.
//! Ported from the slice of `ra8_fs_fat_check.c` that both the FAT and exFAT
//! halves share (RA8FW-1108); the C file keeps the FAT passes and the public
//! entry point.

const c = @import("fs_c.zig").c;

const first_data: u32 = @intCast(c.k_cluster_first_data);
const bit_mask: u32 = 7;
const byte_shift: u5 = 3;

const bad_fat_value: u8 = @intCast(c.k_ra8_fs_check_fault_bad_fat_value);
const crosslink: u8 = @intCast(c.k_ra8_fs_check_fault_crosslink);
const bad_dir_entry: u8 = @intCast(c.k_ra8_fs_check_fault_bad_dir_entry);

/// Record one consistency finding into the report. The first finding is
/// stamped into `first_fault`; later findings only raise the total, so the
/// report always names the earliest thing the scan tripped on.
pub export fn priv_check_fault(ctx: [*c]c.ra8_fs_check_ctx_t, kind: u8, cluster: u32, lba: u64, entry_off: u32) callconv(.c) void {
    const rep = ctx[0].rep;
    if (rep[0].faults_total == 0) {
        rep[0].first_fault.kind = @intCast(kind);
        rep[0].first_fault.cluster = cluster;
        rep[0].first_fault.lba = lba;
        rep[0].first_fault.entry_off = entry_off;
    }
    rep[0].faults_total += 1;
}

/// True when `cluster` is a real data cluster of the volume under check:
/// the addressable range is `[k_cluster_first_data, k_cluster_first_data +
/// clusters_total)`.
pub export fn priv_check_in_range(ctx: [*c]const c.ra8_fs_check_ctx_t, cluster: u32) callconv(.c) u8 {
    if (cluster < first_data) return 0;
    return @intFromBool((cluster - first_data) < ctx[0].rep[0].clusters_total);
}

/// Test-and-set the visited bit for `cluster` in the scratch bitmap (one bit
/// per data cluster, LSB first, bit index `cluster - k_cluster_first_data`).
/// Returns whether the bit was already set: a second chain reaching the same
/// cluster is a cross-link, and the caller stops its walk.
pub export fn priv_check_mark(ctx: [*c]c.ra8_fs_check_ctx_t, cluster: u32) callconv(.c) u8 {
    const idx = cluster - first_data;
    const byte = idx >> byte_shift;
    const mask = @as(u8, 1) << @as(u3, @truncate(idx & bit_mask));
    if ((ctx[0].bitmap[byte] & mask) != 0) return 1;
    ctx[0].bitmap[byte] |= mask;
    return 0;
}

/// Visit a cluster during a walk: mark it and report a range or cross-link
/// fault. Returns whether the caller must stop walking. A cluster out of
/// range records `oor_kind` (bumping `entries_bad` when that kind is
/// `bad_dir_entry`); an already-visited cluster records a cross-link.
pub export fn priv_check_visit(ctx: [*c]c.ra8_fs_check_ctx_t, cluster: u32, oor_kind: u8) callconv(.c) u8 {
    if (priv_check_in_range(ctx, cluster) == 0) {
        if (oor_kind == bad_dir_entry) {
            ctx[0].rep[0].entries_bad += 1;
        }
        priv_check_fault(ctx, oor_kind, cluster, 0, 0);
        return 1;
    }
    if (priv_check_mark(ctx, cluster) != 0) {
        ctx[0].rep[0].chains_crosslinked += 1;
        priv_check_fault(ctx, crosslink, cluster, 0, 0);
        return 1;
    }
    return 0;
}

/// Zero the scratch visited-bitmap over its valid bit range.
pub export fn priv_check_zero_bitmap(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) void {
    const nbytes = (ctx[0].bitmap_bits + bit_mask) >> byte_shift;
    var i: u32 = 0;
    while (i < nbytes) : (i += 1) {
        ctx[0].bitmap[i] = 0;
    }
}
