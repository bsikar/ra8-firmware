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

// ---------------------------------------------------------------------------
// Reserve / commit: planning and writing a new entry's run of slots.
// ---------------------------------------------------------------------------

const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const chars_per_ent: u32 = @intCast(c.k_lfn_chars_per_ent);
/// Growth-retry cap: the longest run this code asks for is 21 slots, the
/// smallest cluster mounted is 16 slots, so two grows always suffice and the
/// third attempt is slack (ported with the reserve that uses it; the C enum
/// was local to ra8_fs_fat_lfn_write.c).
const grow_max: u32 = 3;

const lfn = @import("ra8_fs_lfn_abi.zig");

extern fn priv_dir_find_free_run(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, need: u32, out: [*c]c.dir_slot_t) callconv(.c) u16;
extern fn priv_byte_copy(dst: [*c]u8, src: [*c]const u8, n: u32) callconv(.c) void;
extern fn priv_sec_walk() callconv(.c) [*c]u8;

/// Decide where a new entry's run goes: classify the leaf name (an 8.3 name
/// packs directly, a long name gets a unique generated alias and a chain of
/// LFN slots), then find a free run, growing the directory and retrying up
/// to the growth cap before reporting the directory full.
pub export fn priv_dir_reserve(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, leaf: [*c]const u8, out: [*c]c.dir_insert_t) callconv(.c) u16 {
    out.*.lfn_entries = 0;
    out.*.nunits = 0;
    const kind = lfn.priv_name_classify(leaf, &out.*.units, &out.*.nunits, &out.*.name83, &out.*.ntres);
    if (kind == c.k_name_kind_invalid) return err_invalid_arg;
    var need: u32 = 1;
    if (kind == c.k_name_kind_long) {
        const aerr = internal_alias_unique(m, loc, &out.*.units, out.*.nunits, &out.*.name83);
        if (aerr != ok) return aerr;
        out.*.ntres = 0; // the chain carries the case; the alias is upper-case
        // Groups of THIRTEEN CODE UNITS: the slot's capacity.
        out.*.lfn_entries = @intCast((out.*.nunits + chars_per_ent - 1) / chars_per_ent);
        need = @as(u32, out.*.lfn_entries) + 1;
    }
    var attempt: u32 = 0;
    while (attempt <= grow_max) : (attempt += 1) {
        const ferr = priv_dir_find_free_run(m, loc, need, &out.*.start);
        if (ferr != err_no_mem) return ferr;
        if (attempt == grow_max) break; // growing again would only link a cluster nothing then uses
        const gerr = internal_dir_grow(m, loc);
        if (gerr != ok) return gerr;
    }
    return err_no_mem;
}

/// Write the reserved run: each LFN slot filled from the name's code units,
/// then the 8.3 entry from the caller's template with the packed name and
/// NTRes overlaid, reporting where the 8.3 entry landed.
pub export fn priv_dir_commit(m: [*c]const c.ra8_fs_mount_t, plan: [*c]const c.dir_insert_t, tmpl: [*c]const u8, out_lba: [*c]u64, out_off: [*c]u32) callconv(.c) u16 {
    const nlen: u32 = if (plan.*.lfn_entries != 0) plan.*.nunits else 0;
    const csum = lfn.priv_sfn_checksum(&plan.*.name83);
    const total: u32 = @as(u32, plan.*.lfn_entries) + 1;
    var cur: c.dir_slot_t = plan.*.start;
    var lba: u64 = cur.w.cur_lba;
    const buf: [*c]u8 = priv_sec_walk();
    var err = priv_read_sector(m, lba, buf);
    if (err != ok) return err;
    var i: u32 = 0;
    while (i < total) : (i += 1) {
        const slot: [*c]u8 = buf + @as(usize, cur.ent) * @as(usize, entry_bytes);
        if (i < plan.*.lfn_entries) {
            lfn.priv_lfn_fill_slot(slot, &plan.*.units, nlen, @as(u32, plan.*.lfn_entries) - i, if (i == 0) @as(u8, 1) else @as(u8, 0), csum);
        } else {
            priv_byte_copy(slot, tmpl, entry_bytes);
            priv_byte_copy(slot + @as(u32, @intCast(c.k_dir_off_name)), &plan.*.name83, @intCast(c.k_dir_name_field_len));
            slot[@intCast(c.k_dir_off_ntres)] = plan.*.ntres;
            out_lba.* = lba;
            out_off.* = cur.ent * entry_bytes;
        }
        if (i + 1 < total) {
            err = internal_slot_advance(m, &cur, buf, &lba);
            if (err != ok) return err;
        }
    }
    return priv_write_sector(m, lba, buf);
}
