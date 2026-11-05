//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared scaffolding for the read-only consistency check (fsck): the finding
//! recorder, the visited-cluster bitmap primitives, and the walk-stop helper.
//! Ported from the slice of `ra8_fs_fat_check.c` that both the FAT and exFAT
//! halves share (RA8FW-1108); the C file keeps the FAT passes and the public
//! entry point.

const std = @import("std");
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

// ---------------------------------------------------------------------------
// Pass 1: classify every FAT entry (RA8FW-1109)
// ---------------------------------------------------------------------------

/// The FAT defective-cluster marker for the mounted variant (MS FAT spec
/// sec 4): one below the end-of-chain threshold.
const bad_fat12: u32 = 0x0FF7;
const bad_fat16: u32 = 0xFFF7;
const bad_fat32: u32 = 0x0FFF_FFF7;

const cluster_free: u32 = @intCast(c.k_cluster_free);

/// The defective-cluster marker for the mounted FAT variant.
pub export fn internal_fat_bad_marker(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32 {
    if (m[0].type == c.k_ra8_fs_type_fat12) return bad_fat12;
    if (m[0].type == c.k_ra8_fs_type_fat16) return bad_fat16;
    return bad_fat32;
}

/// Classify one FAT entry value into the report's cluster tallies. Free,
/// defective and end-of-chain are their own tallies; a value inside the data
/// range counts used; the reserved 1 or a pointer past the last cluster
/// counts used and raises a bad-FAT-value fault.
pub export fn internal_fat_classify_one(ctx: [*c]c.ra8_fs_check_ctx_t, cluster: u32, value: u32) callconv(.c) void {
    if (value == cluster_free) {
        ctx[0].rep[0].clusters_free += 1;
        return;
    }
    if (value == internal_fat_bad_marker(ctx[0].m)) {
        ctx[0].rep[0].clusters_bad += 1;
        return;
    }
    ctx[0].rep[0].clusters_used += 1;
    if (c.priv_is_eoc(ctx[0].m, value) != 0) return;
    if (priv_check_in_range(ctx, value) == 0) {
        priv_check_fault(ctx, bad_fat_value, cluster, 0, 0);
    }
}

/// Pass 1: read and classify every data cluster's FAT entry, one bounded
/// sweep over the whole FAT.
pub export fn internal_fat_classify(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    const last = first_data + ctx[0].rep[0].clusters_total;
    var clus: u32 = first_data;
    while (clus < last) : (clus += 1) {
        var v: u32 = 0;
        const err: u16 = @intCast(c.priv_fat_get(ctx[0].m, clus, &v));
        if (err != c.k_ra8_ok) return err;
        internal_fat_classify_one(ctx, clus, v);
    }
    return @intCast(c.k_ra8_ok);
}

// ---------------------------------------------------------------------------
// Pass 2: chain marking and the directory worklist (RA8FW-1110)
// ---------------------------------------------------------------------------

const bad_chain: u8 = @intCast(c.k_ra8_fs_check_fault_bad_chain);
const scan_truncated: u8 = @intCast(c.k_ra8_fs_check_fault_scan_truncated);

/// Mark one file's whole cluster chain: visit each cluster, follow the FAT
/// toward the end of chain, and stop on a cross-link, a broken next-pointer,
/// or the `clusters_total` hop bound.
pub export fn internal_fat_mark_chain(ctx: [*c]c.ra8_fs_check_ctx_t, first: u32) callconv(.c) u16 {
    var clus = first;
    var hop: u32 = 0;
    while (hop < ctx[0].rep[0].clusters_total) : (hop += 1) {
        if (priv_check_visit(ctx, clus, bad_chain) != 0) return @intCast(c.k_ra8_ok);
        var v: u32 = 0;
        const err: u16 = @intCast(c.priv_fat_get(ctx[0].m, clus, &v));
        if (err != c.k_ra8_ok) return err;
        if (c.priv_is_eoc(ctx[0].m, v) != 0) return @intCast(c.k_ra8_ok);
        if (priv_check_in_range(ctx, v) == 0) {
            priv_check_fault(ctx, bad_chain, clus, 0, 0);
            return @intCast(c.k_ra8_ok);
        }
        clus = v;
    }
    _ = priv_check_visit(ctx, clus, bad_chain);
    return @intCast(c.k_ra8_ok);
}

/// Push a subdirectory onto the walk stack, capping the depth: the first
/// overflow drops the entry and records the truncation fault exactly once.
pub export fn internal_fat_push(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.fat_dir_stack_t, clus: u32) callconv(.c) void {
    if (stack[0].top >= @as(u32, @intCast(c.k_ra8_fs_check_max_dirs))) {
        if (stack[0].truncated == 0) {
            stack[0].truncated = 1;
            priv_check_fault(ctx, scan_truncated, clus, 0, 0);
        }
        return;
    }
    stack[0].items[stack[0].top].is_root = 0;
    stack[0].items[stack[0].top].cluster = clus;
    stack[0].top += 1;
}

// ---------------------------------------------------------------------------
// Pass 2: the directory-entry processor and the per-sector walk (RA8FW-1111)
// ---------------------------------------------------------------------------

const dir_off_name: u32 = @intCast(c.k_dir_off_name);
const dir_off_attr: u32 = @intCast(c.k_dir_off_attr);
const dir_entry_bytes: u32 = @intCast(c.k_ra8_fs_dir_entry_bytes);

const marker_free_perm: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);
const marker_dot: u8 = @intCast(c.k_dir_marker_dot);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_volume_id: u8 = @intCast(c.k_ra8_fs_attr_volume_id);

/// Process one 32-byte FAT directory entry: skip end-of-directory, deleted,
/// long-name, dot and volume-label entries, then validate a real file's or
/// directory's first cluster. A file's chain is marked at once; a directory
/// is queued for the worklist walk.
pub export fn internal_fat_entry(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.fat_dir_stack_t, ent: [*c]const u8, lba: u64, entry_off: u32, out_eod: [*c]u8) callconv(.c) u16 {
    out_eod.* = 0;
    const name0 = ent[dir_off_name];
    const attr = ent[dir_off_attr];
    if (name0 == marker_free_perm) {
        out_eod.* = 1;
        return @intCast(c.k_ra8_ok);
    }
    if (name0 == marker_free_used) return @intCast(c.k_ra8_ok);
    if (attr == attr_lfn) return @intCast(c.k_ra8_ok);
    if (name0 == marker_dot) return @intCast(c.k_ra8_ok);
    const is_dir: u8 = @intFromBool((attr & attr_directory) != 0);
    if (is_dir == 0 and (attr & attr_volume_id) != 0) return @intCast(c.k_ra8_ok);
    const first: u32 = @intCast(c.priv_entry_first_cluster(ent));
    if (first != 0 and priv_check_in_range(ctx, first) == 0) {
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_dir_entry, first, lba, entry_off);
        return @intCast(c.k_ra8_ok);
    }
    if (is_dir != 0) {
        if (first != 0) internal_fat_push(ctx, stack, first);
        return @intCast(c.k_ra8_ok);
    }
    ctx[0].rep[0].files_visited += 1;
    if (first != 0) return internal_fat_mark_chain(ctx, first);
    return @intCast(c.k_ra8_ok);
}

/// Process the directory entries packed into one sector: walk the entries and
/// stop at the first end-of-directory marker.
pub export fn internal_fat_visit_sector(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.fat_dir_stack_t, buf: [*c]const u8, lba: u64, out_eod: [*c]u8) callconv(.c) u16 {
    var e: u32 = 0;
    while (e < c.priv_dir_eps(ctx[0].m)) : (e += 1) {
        const off = e * dir_entry_bytes;
        var eod: u8 = 0;
        const err: u16 = @intCast(internal_fat_entry(ctx, stack, buf + off, lba, off, &eod));
        if (err != c.k_ra8_ok) return err;
        if (eod != 0) {
            out_eod.* = 1;
            return @intCast(c.k_ra8_ok);
        }
    }
    out_eod.* = 0;
    return @intCast(c.k_ra8_ok);
}

// ---------------------------------------------------------------------------
// Pass 2: the tree scan (RA8FW-1112)
// ---------------------------------------------------------------------------

/// Walk the FAT12/16 fixed root-directory region, sector by sector, until the
/// end-of-directory marker or the region's last sector.
pub export fn internal_fat_scan_fixed_root(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.fat_dir_stack_t) callconv(.c) u16 {
    const eps = c.priv_dir_eps(ctx[0].m);
    const secs = (ctx[0].m[0].root_entries + eps - 1) / eps;
    var s: u32 = 0;
    while (s < secs) : (s += 1) {
        const lba: u64 = @intCast(ctx[0].m[0].first_root_lba + @as(u64, s));
        const buf = c.priv_sec_walk();
        const err: u16 = @intCast(c.priv_read_sector(ctx[0].m, lba, buf));
        if (err != c.k_ra8_ok) return err;
        var eod: u8 = 0;
        const verr: u16 = @intCast(internal_fat_visit_sector(ctx, stack, buf, lba, &eod));
        if (verr != c.k_ra8_ok) return verr;
        if (eod != 0) return @intCast(c.k_ra8_ok);
    }
    return @intCast(c.k_ra8_ok);
}

/// Walk a cluster-chained directory (a FAT32 root or any subdirectory): mark
/// each directory cluster, process every sector's entries, and follow the
/// chain to the end-of-directory marker, a terminal FAT entry, or the hop
/// bound with the pending successor visited once more.
pub export fn internal_fat_scan_cluster_dir(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.fat_dir_stack_t, first: u32) callconv(.c) u16 {
    var clus = first;
    var hop: u32 = 0;
    while (hop < ctx[0].rep[0].clusters_total) : (hop += 1) {
        if (priv_check_visit(ctx, clus, bad_dir_entry) != 0) return @intCast(c.k_ra8_ok);
        const base = c.priv_cluster_to_lba(ctx[0].m, clus);
        var s: u32 = 0;
        while (s < ctx[0].m[0].sectors_per_cluster) : (s += 1) {
            const buf = c.priv_sec_walk();
            const err: u16 = @intCast(c.priv_read_sector(ctx[0].m, base + @as(u64, s), buf));
            if (err != c.k_ra8_ok) return err;
            var eod: u8 = 0;
            const verr: u16 = @intCast(internal_fat_visit_sector(ctx, stack, buf, base + @as(u64, s), &eod));
            if (verr != c.k_ra8_ok) return verr;
            if (eod != 0) return @intCast(c.k_ra8_ok);
        }
        var v: u32 = 0;
        const ferr: u16 = @intCast(c.priv_fat_get(ctx[0].m, clus, &v));
        if (ferr != c.k_ra8_ok) return ferr;
        if (c.priv_is_eoc(ctx[0].m, v) != 0) return @intCast(c.k_ra8_ok);
        clus = v;
    }
    _ = priv_check_visit(ctx, clus, bad_dir_entry);
    return @intCast(c.k_ra8_ok);
}

/// Pass 2's driver: walk the whole directory tree from the root with the
/// explicit worklist, popping queued directories until it empties.
pub export fn internal_fat_tree(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    stack.items[0].is_root = 1;
    stack.top = 1;
    const fat32 = ctx[0].m[0].type == c.k_ra8_fs_type_fat32;
    while (stack.top > 0) {
        stack.top -= 1;
        const loc = stack.items[stack.top];
        ctx[0].rep[0].dirs_visited += 1;
        var err: u16 = @intCast(c.k_ra8_ok);
        if (loc.is_root != 0 and !fat32) {
            err = internal_fat_scan_fixed_root(ctx, &stack);
        } else {
            const first: u32 = if (loc.is_root != 0) ctx[0].m[0].root_cluster else loc.cluster;
            err = internal_fat_scan_cluster_dir(ctx, &stack, first);
        }
        if (err != c.k_ra8_ok) return err;
    }
    return @intCast(c.k_ra8_ok);
}

// ---------------------------------------------------------------------------
// Pass 3 and the FAT orchestrator (RA8FW-1113)
// ---------------------------------------------------------------------------

const lost_cluster: u8 = @intCast(c.k_ra8_fs_check_fault_lost_cluster);
const free_count_bad: u8 = @intCast(c.k_ra8_fs_check_fault_free_count_bad);
const fsi_off_lead: usize = @intCast(c.k_fmt_fsi_off_lead);
const fsi_off_struct: usize = @intCast(c.k_fmt_fsi_off_struct);
const fsi_off_trail: usize = @intCast(c.k_fmt_fsi_off_trail);
const fsi_off_free: usize = @intCast(c.k_fmt_fsi_off_free);
const fsi_lead_sig: u32 = @intCast(c.k_fmt_fsi_lead_sig);
const fsi_struct_sig: u32 = @intCast(c.k_fmt_fsi_struct_sig);
const fsi_trail_sig: u32 = @intCast(c.k_fmt_fsi_trail_sig);
const off_f32_fsinfo: usize = @intCast(c.k_fmt_off_f32_fsinfo);

extern fn priv_rd16(p: [*c]const u8) u16;
extern fn priv_rd32(p: [*c]const u8) u32;

/// Pass 3: flag allocated clusters no directory chain reached, in one pass
/// over the FAT after the tree walk has run.
pub export fn internal_fat_diff(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    const marker = internal_fat_bad_marker(ctx[0].m);
    const last = @as(u32, @intCast(c.k_cluster_first_data)) + ctx[0].rep[0].clusters_total;
    var clus: u32 = @intCast(c.k_cluster_first_data);
    while (clus < last) : (clus += 1) {
        var v: u32 = 0;
        const err: u16 = @intCast(c.priv_fat_get(ctx[0].m, clus, &v));
        if (err != c.k_ra8_ok) return err;
        if (v == c.k_cluster_free or v == marker) continue;
        const idx = clus - first_data;
        const mask = @as(u8, 1) << @intCast(idx & bit_mask);
        if (ctx[0].bitmap[idx >> byte_shift] & mask == 0) {
            ctx[0].rep[0].clusters_lost += 1;
            priv_check_fault(ctx, lost_cluster, clus, 0, 0);
        }
    }
    return @intCast(c.k_ra8_ok);
}

/// Compare a FAT32 volume's FSInfo free count against the classify pass. A
/// volume with no or an untrusted FSInfo sector is not a fault: there is
/// simply nothing to compare.
pub export fn internal_fat_fsinfo(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    if (ctx[0].m[0].type != c.k_ra8_fs_type_fat32) return @intCast(c.k_ra8_ok);
    const boot = c.priv_sec_walk();
    const be: u16 = @intCast(c.priv_read_sector(ctx[0].m, 0, boot));
    if (be != c.k_ra8_ok) return be;
    const lba: u32 = priv_rd16(boot + off_f32_fsinfo);
    if (lba == 0 or lba >= ctx[0].m[0].reserved_sectors) return @intCast(c.k_ra8_ok);
    const sec = c.priv_sec_walk();
    const se: u16 = @intCast(c.priv_read_sector(ctx[0].m, lba, sec));
    if (se != c.k_ra8_ok) return se;
    if (priv_rd32(sec + fsi_off_lead) != fsi_lead_sig) return @intCast(c.k_ra8_ok);
    if (priv_rd32(sec + fsi_off_struct) != fsi_struct_sig) return @intCast(c.k_ra8_ok);
    if (priv_rd32(sec + fsi_off_trail) != fsi_trail_sig) return @intCast(c.k_ra8_ok);
    const stored = priv_rd32(sec + fsi_off_free);
    if (stored != c.k_fs_free_unknown and stored != ctx[0].rep[0].clusters_free) {
        priv_check_fault(ctx, free_count_bad, 0, lba, 0);
    }
    return @intCast(c.k_ra8_ok);
}

/// The FAT half of the read-only consistency check: classify the FAT, then
/// with a bitmap walk the tree, diff for lost clusters, and compare the
/// FSInfo free count.
pub export fn priv_check_fat(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    var err: u16 = @intCast(internal_fat_classify(ctx));
    if (err != c.k_ra8_ok) return err;
    if (ctx[0].bitmap != null) {
        priv_check_zero_bitmap(ctx);
        err = internal_fat_tree(ctx);
        if (err != c.k_ra8_ok) return err;
        err = internal_fat_diff(ctx);
        if (err != c.k_ra8_ok) return err;
    }
    return internal_fat_fsinfo(ctx);
}
