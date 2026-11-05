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
const exfat_hash = @import("ra8_fs_exfat_hash_abi.zig");

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

// ---------------------------------------------------------------------------
// exFAT check: marking referenced clusters and entry-set verification
// (RA8FW-1114)
// ---------------------------------------------------------------------------

const bad_set_checksum: u8 = @intCast(c.k_ra8_fs_check_fault_bad_set_checksum);
const bad_name_hash: u8 = @intCast(c.k_ra8_fs_check_fault_bad_name_hash);

const name_cap: u32 = @intCast(c.k_exfat_name_cap);
const name_per_entry: u32 = @intCast(c.k_exfat_name_per_entry);
const entry_bytes: u32 = @intCast(c.k_exfat_entry_bytes);

/// Mark a contiguous cluster run visited (a NoFatChain file's run or a
/// bitmap / up-case system run), detecting cross-links. A cluster out of
/// range is a bad entry. A run longer than the in-volume suffix starting at
/// `first` is faulted before the bounded in-range prefix is marked, so
/// clamping a corrupt DataLength can never turn it into a clean run.
pub export fn internal_exchk_mark_run(ctx: [*c]c.ra8_fs_check_ctx_t, first: u32, nclus: u64) callconv(.c) void {
    if (priv_check_in_range(ctx, first) == 0) {
        _ = priv_check_visit(ctx, first, bad_dir_entry);
        return;
    }
    var n: u32 = @truncate(nclus);
    const available: u32 = ctx[0].rep[0].clusters_total - (first - first_data);
    if (nclus > @as(u64, available)) {
        n = available;
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_dir_entry, first + n, 0, 0);
    }
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        if (priv_check_visit(ctx, first + i, bad_dir_entry) != 0) return;
    }
}

/// Follow a fragmented file's FAT chain, marking each cluster and flagging a
/// revisit (cross-link) or a next-cluster out of range (bad chain). A
/// non-terminal successor after exactly `clusters_total` visits is checked
/// once more, which necessarily exposes a revisit or bad tail.
pub export fn internal_exchk_mark_fatchain(ctx: [*c]c.ra8_fs_check_ctx_t, first: u32) callconv(.c) u16 {
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

/// Walk a directory's own run (contiguous or FAT-chained, decided by
/// `contig_end`) with the real step helper, marking each cluster, so a
/// directory whose contents stop at an early end-of-directory marker still
/// counts its trailing allocated clusters as referenced. A successor
/// remaining after the last permitted hop is validated as a cross-link or
/// bad entry.
pub export fn internal_exchk_mark_dir_alloc(ctx: [*c]c.ra8_fs_check_ctx_t, dir: [*c]const c.exfat_dir_t) callconv(.c) u16 {
    var clus = dir[0].cluster;
    var hop: u32 = 0;
    while (hop < ctx[0].rep[0].clusters_total) : (hop += 1) {
        if (priv_check_visit(ctx, clus, bad_dir_entry) != 0) return @intCast(c.k_ra8_ok);
        var next: u32 = 0;
        const e: u16 = @intCast(c.priv_exfat_step_cluster(ctx[0].m, clus, dir[0].contig_end, &next));
        if (e == @as(u16, @intCast(c.k_ra8_err_not_found))) return @intCast(c.k_ra8_ok);
        if (e != c.k_ra8_ok) return e;
        clus = next;
    }
    _ = priv_check_visit(ctx, clus, bad_dir_entry);
    return @intCast(c.k_ra8_ok);
}

/// Mark the clusters a bitmap (0x81) or up-case (0x82) system entry owns:
/// ceil(DataLength / cluster bytes) clusters from FirstCluster, both read
/// from the same fields a Stream entry uses.
pub export fn internal_exchk_system_run(ctx: [*c]c.ra8_fs_check_ctx_t, e: [*c]const u8) callconv(.c) void {
    const first: u32 = @intCast(c.priv_rd32(e + @as(usize, @intCast(c.k_exfat_strm_off_clus))));
    const bytes: u64 = @intCast(c.priv_rd64(e + @as(usize, @intCast(c.k_exfat_strm_off_dlen))));
    const cbytes: u32 = @intCast(c.priv_cluster_bytes(ctx[0].m));
    const nclus: u64 = (bytes / cbytes) + @intFromBool(bytes % cbytes != 0);
    if (first == 0) return;
    internal_exchk_mark_run(ctx, first, nclus);
}

const off_file_csum: usize = @intCast(c.k_exfat_off_file_csum);
const off_strm_hash: usize = @intCast(c.k_exfat_off_strm_hash);
const strm_off_nlen: usize = @intCast(c.k_exfat_strm_off_nlen);
const name_off: usize = @intCast(c.k_exfat_name_off);

extern fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) u16;

/// Pull the Name entries' UTF-16 units out of a file entry set: unit `i`
/// lives in name entry `2 + i / name_per_entry`, at column `i %
/// name_per_entry`, two bytes per unit from the entry's name offset.
pub export fn internal_exchk_extract_name(set: [*c]const u8, nlen: u32, units: [*c]u16) callconv(.c) void {
    var i: u32 = 0;
    while (i < nlen) : (i += 1) {
        const ent = 2 + i / name_per_entry;
        const col = i % name_per_entry;
        const off: usize = ent * entry_bytes + name_off + col * 2;
        units[i] = priv_rd16(set + off);
    }
}

/// Verify one file entry set: the SetChecksum over the whole set and the
/// stream entry's NameHash over the extracted name. `nlen` is clamped to the
/// name cap first, so a corrupt length can never read past the set's name
/// entries. Each mismatch is its own finding, both stamped at the set's
/// directory offset.
pub export fn internal_exchk_verify_set(ctx: [*c]c.ra8_fs_check_ctx_t, set: [*c]const u8, count: u32, lba: u64, off: u32) callconv(.c) void {
    const stored_cs: u16 = priv_rd16(set + off_file_csum);
    const calc_cs: u16 = priv_exfat_set_checksum(set, count * entry_bytes);
    if (stored_cs != calc_cs) {
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_set_checksum, 0, lba, off);
    }
    const strm: [*c]const u8 = set + @as(usize, entry_bytes);
    var nlen: u32 = strm[strm_off_nlen];
    if (nlen > name_cap) {
        nlen = name_cap;
    }
    var units: [name_cap]u16 = @splat(0);
    internal_exchk_extract_name(set, nlen, &units);
    const stored_hash: u16 = priv_rd16(strm + off_strm_hash);
    const calc_hash: u16 = exfat_hash.priv_exfat_name_hash(&units, nlen);
    if (stored_hash != calc_hash) {
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_name_hash, 0, lba, off);
    }
}

// =============================================================================
// Directory-tree walk
// =============================================================================

const off_file_secnt: usize = @intCast(c.k_exfat_off_file_secnt);
const off_file_attr: usize = @intCast(c.k_exfat_off_file_attr);
const strm_off_flags: usize = @intCast(c.k_exfat_strm_off_flags);
const strm_off_clus: usize = @intCast(c.k_exfat_strm_off_clus);
const strm_off_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
const secflag_no_fat: u8 = @intCast(c.k_exfat_secflag_no_fat);
const set_max_entries: u32 = @intCast(c.k_exfat_set_max_entries);
const scan_limit: u32 = @intCast(c.k_exfat_scan_limit);
const entry_eod: u8 = @intCast(c.k_exfat_entry_eod);
const entry_bitmap: u8 = @intCast(c.k_exfat_entry_bitmap);
const entry_upcase: u8 = @intCast(c.k_exfat_entry_upcase);
const entry_file: u8 = @intCast(c.k_exfat_entry_file);
const inuse_bit: u8 = @intCast(c.k_exfat_inuse_bit);
const max_dirs: u32 = @intCast(c.k_ra8_fs_check_max_dirs);

/// Queue an exFAT subdirectory onto the walk worklist, capping the depth:
/// one push past the ceiling records a single scan-truncation fault and the
/// entry is dropped.
pub export fn internal_exchk_push(ctx: [*c]c.ra8_fs_check_ctx_t, stack: [*c]c.exfat_dir_stack_t, dir: [*c]const c.exfat_dir_t) callconv(.c) void {
    if (stack[0].top >= max_dirs) {
        if (stack[0].truncated == 0) {
            stack[0].truncated = 1;
            priv_check_fault(ctx, scan_truncated, dir[0].cluster, 0, 0);
        }
        return;
    }
    stack[0].items[stack[0].top] = dir[0];
    stack[0].top += 1;
}

/// The cluster half of the entry-set walk: read the Stream entry, validate the
/// first cluster, then count an empty file, fault an out-of-range entry, queue
/// a subdirectory, or mark a file's run (contiguous or FAT-chained).
pub export fn internal_exchk_set_clusters(ctx: [*c]c.ra8_fs_check_ctx_t, set: [*c]const u8, file: [*c]const u8, stack: [*c]c.exfat_dir_stack_t, lba: u64, off: u32) callconv(.c) u16 {
    const strm: [*c]const u8 = set + @as(usize, entry_bytes);
    const first: u32 = priv_rd32(strm + strm_off_clus);
    const dlen: u64 = c.priv_rd64(strm + strm_off_dlen);
    const is_dir: u8 = if ((file[off_file_attr] & attr_directory) != 0) 1 else 0;
    if (first == 0) {
        if (is_dir == 0) {
            ctx[0].rep[0].files_visited += 1;
        }
        return @intCast(c.k_ra8_ok);
    }
    if (priv_check_in_range(ctx, first) == 0) {
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_dir_entry, first, lba, off);
        return @intCast(c.k_ra8_ok);
    }
    if (is_dir != 0) {
        var sub: c.exfat_dir_t = undefined;
        c.priv_exfat_dir_from_set(ctx[0].m, strm, &sub);
        internal_exchk_push(ctx, stack, &sub);
        return @intCast(c.k_ra8_ok);
    }
    ctx[0].rep[0].files_visited += 1;
    const cbytes: u32 = @intCast(c.priv_cluster_bytes(ctx[0].m));
    const nclus: u64 = if (dlen == 0) 1 else (dlen / @as(u64, cbytes)) + @intFromBool(dlen % @as(u64, cbytes) != 0);
    if ((strm[strm_off_flags] & secflag_no_fat) != 0) {
        internal_exchk_mark_run(ctx, first, nclus);
        return @intCast(c.k_ra8_ok);
    }
    return internal_exchk_mark_fatchain(ctx, first);
}

/// Gather and process one File entry set: assemble it from the cursor,
/// verify it, then mark or queue it. A SecondaryCount outside the writable
/// range is a malformed set: faulted and skipped, and the leftover secondary
/// entries the outer walk then sees are ignored as non-File types.
pub export fn internal_exchk_set(ctx: [*c]c.ra8_fs_check_ctx_t, cur: [*c]c.exfat_cursor_t, file: [*c]const u8, stack: [*c]c.exfat_dir_stack_t) callconv(.c) u16 {
    const count: u32 = 1 + @as(u32, file[off_file_secnt]);
    const bic: u32 = (cur[0].entry_in_cluster - 1) * entry_bytes;
    const lba: u64 = @as(u64, @intCast(c.priv_cluster_to_lba(ctx[0].m, cur[0].cluster))) + @as(u64, bic / @as(u32, @intCast(c.priv_bps(ctx[0].m))));
    const off: u32 = bic % @as(u32, @intCast(c.priv_bps(ctx[0].m)));
    if (count < 2 or count > set_max_entries) {
        ctx[0].rep[0].entries_bad += 1;
        priv_check_fault(ctx, bad_dir_entry, 0, lba, off);
        return @intCast(c.k_ra8_ok);
    }
    var set: [set_max_entries * entry_bytes]u8 = @splat(0);
    c.priv_byte_copy(&set, file, entry_bytes);
    var k: u32 = 1;
    while (k < count) : (k += 1) {
        const e: u16 = @intCast(c.priv_exfat_next_entry(ctx[0].m, cur, &set[@as(usize, k) * entry_bytes]));
        if (e == @as(u16, @intCast(c.k_ra8_err_not_found))) {
            ctx[0].rep[0].entries_bad += 1;
            priv_check_fault(ctx, bad_dir_entry, 0, lba, off);
            return @intCast(c.k_ra8_ok);
        }
        if (e != @as(u16, @intCast(c.k_ra8_ok))) {
            return e;
        }
    }
    internal_exchk_verify_set(ctx, &set, count, lba, off);
    return internal_exchk_set_clusters(ctx, &set, file, stack, lba, off);
}

/// Dispatch one live File entry: a set whose secondary entries would run past
/// the scan ceiling is a walk truncation, reported and stopped here rather
/// than inside set assembly. `out_stop` carries whether the caller's scan
/// loop must return immediately with the returned status.
pub export fn internal_exchk_scan_dir_file_entry(ctx: [*c]c.ra8_fs_check_ctx_t, cur: [*c]c.exfat_cursor_t, e: [*c]const u8, stack: [*c]c.exfat_dir_stack_t, dir_cluster: u32, out_stop: [*c]u8) callconv(.c) u16 {
    const count: u32 = 1 + @as(u32, e[off_file_secnt]);
    const remaining: u32 = scan_limit - cur[0].scanned;
    if (count <= set_max_entries) {
        if (count > 1) {
            if ((count - 1) > remaining) {
                priv_check_fault(ctx, scan_truncated, dir_cluster, 0, 0);
                out_stop[0] = 1;
                return @intCast(c.k_ra8_ok);
            }
        }
    }
    const se: u16 = internal_exchk_set(ctx, cur, e, stack);
    out_stop[0] = if (se != @as(u16, @intCast(c.k_ra8_ok))) 1 else 0;
    return se;
}

/// Probe exactly one entry past the scan ceiling to classify the walk: an
/// exhausted allocation or an immediate end-of-directory marker proves the
/// directory ended exactly on the scan bound; any other live or deleted entry
/// proves the walk was truncated.
pub export fn internal_exchk_scan_dir_terminal(ctx: [*c]c.ra8_fs_check_ctx_t, cur: [*c]c.exfat_cursor_t, dir: [*c]const c.exfat_dir_t) callconv(.c) u16 {
    var terminal: [entry_bytes]u8 = @splat(0);
    const probe: u16 = @intCast(c.priv_exfat_next_entry(ctx[0].m, cur, &terminal));
    if (probe == @as(u16, @intCast(c.k_ra8_err_not_found))) {
        return @intCast(c.k_ra8_ok);
    }
    if (probe != @as(u16, @intCast(c.k_ra8_ok))) {
        return probe;
    }
    if (terminal[0] == entry_eod) {
        return @intCast(c.k_ra8_ok);
    }
    priv_check_fault(ctx, scan_truncated, dir[0].cluster, 0, 0);
    return @intCast(c.k_ra8_ok);
}

/// Walk one exFAT directory, verifying and marking every live entry: system,
/// file and directory sets dispatch; at the entry ceiling one terminal probe
/// separates an exact-bound directory from a truncated walk.
pub export fn internal_exchk_scan_dir(ctx: [*c]c.ra8_fs_check_ctx_t, dir: [*c]const c.exfat_dir_t, stack: [*c]c.exfat_dir_stack_t) callconv(.c) u16 {
    var cur: c.exfat_cursor_t = std.mem.zeroes(c.exfat_cursor_t);
    c.priv_exfat_cursor_init(dir, &cur);
    while (cur.scanned < scan_limit) {
        var e: [entry_bytes]u8 = @splat(0);
        const r: u16 = @intCast(c.priv_exfat_next_entry(ctx[0].m, &cur, &e));
        if (r == @as(u16, @intCast(c.k_ra8_err_not_found))) {
            return @intCast(c.k_ra8_ok); // run ended without an end-of-directory marker
        }
        if (r != @as(u16, @intCast(c.k_ra8_ok))) {
            return r;
        }
        if (e[0] == entry_eod) {
            return @intCast(c.k_ra8_ok);
        }
        if ((e[0] & inuse_bit) == 0) {
            continue; // a deleted remnant
        }
        if (e[0] == entry_bitmap or e[0] == entry_upcase) {
            internal_exchk_system_run(ctx, &e);
            continue;
        }
        if (e[0] == entry_file) {
            var stop: u8 = 0;
            const fe: u16 = internal_exchk_scan_dir_file_entry(ctx, &cur, &e, stack, dir[0].cluster, &stop);
            if (stop != 0) {
                return fe;
            }
        }
    }
    return internal_exchk_scan_dir_terminal(ctx, &cur, dir);
}

/// The explicit DFS array (~2 KiB) stays in module-static storage so the
/// frame stays within budget; the walk is iterative and single-threaded
/// under the fs lock.
var s_exchk_work_items: [max_dirs]c.exfat_dir_t = undefined;

/// Walk the whole exFAT directory tree from the root with a worklist: pop
/// directories, mark each one's own run, then scan its entries.
pub export fn internal_exchk_tree(ctx: [*c]c.ra8_fs_check_ctx_t) callconv(.c) u16 {
    var worklist: c.exfat_dir_stack_t = undefined;
    worklist.items = &s_exchk_work_items;
    worklist.top = 0;
    worklist.truncated = 0;
    c.priv_exfat_dir_root(ctx[0].m, &worklist.items[0]);
    worklist.top = 1;
    while (worklist.top > 0) {
        worklist.top -= 1;
        const dir: c.exfat_dir_t = worklist.items[worklist.top];
        ctx[0].rep[0].dirs_visited += 1;
        var err: u16 = internal_exchk_mark_dir_alloc(ctx, &dir);
        if (err != @as(u16, @intCast(c.k_ra8_ok))) {
            return err;
        }
        err = internal_exchk_scan_dir(ctx, &dir, &worklist);
        if (err != @as(u16, @intCast(c.k_ra8_ok))) {
            return err;
        }
    }
    return @intCast(c.k_ra8_ok);
}
