//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VFAT long-name directory scan: the per-sector match and the walk that
//! locates a file's 8.3 entry by its long name.
//!
//! A long name is stored as a chain of attr-0x0F entries immediately before
//! the 8.3 short entry, in reverse order: the entry tagged 0x40 (last logical
//! group) is physically first, then group N-1 ... group 1, then the 8.3
//! entry. Each LFN entry carries 13 UTF-16LE chars (offsets 1/3/5/7/9,
//! 14/16/18/20/22/24, 28/30) and a checksum of the 8.3 name (offset 13) that
//! ties the chain to its entry. The layout table and the chain helpers live
//! in `ra8_fs_lfn_abi.zig`; the verbs that decide WHEN to write a chain live
//! in `ra8_fs_dir_write_abi.zig`.
//!
//! Names are compared as UTF-16, which is the domain they are stored in and
//! the domain the up-case table folds. Comparing reassembled text meant
//! comparing against a name the reader had already mangled, so a file whose
//! name held an accent could not be opened by its real name.
//!
//! The directory walkers (init_loc / next_sector) stay in the C library;
//! this unit reaches them, and every C type it touches, through `fs_c.zig`.
//! The sector read itself is already Zig (`ra8_fs_fat_prim_abi.zig`).

const std = @import("std");
const c = @import("fs_c.zig").c;
const lf = @import("ra8_fs_lfn_abi.zig");
const utf = @import("ra8_fs_utf_abi.zig");

pub const LfnState = lf.LfnState;

pub const ok: u16 = c.k_ra8_ok;
pub const err_not_found: u16 = c.k_ra8_err_not_found;

const Mount = c.ra8_fs_mount_t;
const dirent_len: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const name_off: usize = @intCast(c.k_dir_off_name);
const attr_off: usize = @intCast(c.k_dir_off_attr);
const marker_eod: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_deleted: u8 = @intCast(c.k_dir_marker_free_used);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);

/// Outcome of scanning one directory sector for a long-name match: lets the
/// walk report "keep walking", "found", or "end of directory reached" without
/// unwinding the caller's loop state.
pub const Scan = enum(u8) {
    /// No match in this sector; advance to the next.
    more,
    /// Long name matched; out parameters populated.
    found,
    /// End-of-directory marker hit; stop the walk.
    eod,
};

/// Scan one directory sector for a long-name match, updating the chain.
///
/// Folds any LFN sub-entries into `s` and, on the trailing 8.3 entry, compares
/// its reassembled long name against `needle`. The per-sector body of the
/// walk in `priv_dir_find_long`, extracted so the scan and the walk each stay
/// small. `s` must have been reset before the first sector.
pub fn scanSector(
    m: *const Mount,
    needle: [*]const u16,
    nneedle: u32,
    buf: [*]const u8,
    cur_lba: u64,
    s: *LfnState,
    out_lba: *u64,
    out_entry_off: *u32,
    out_entry: [*]u8,
) Scan {
    const eps: u32 = c.priv_dir_eps(m);
    var e: u32 = 0;
    while (e < eps) : (e += 1) {
        const ent = buf + @as(usize, e) * dirent_len;
        if (ent[name_off] == marker_eod) {
            return .eod;
        }
        if (ent[name_off] == marker_deleted) {
            lf.priv_lfn_reset(s); // a deleted slot breaks the chain
            continue;
        }
        if (ent[attr_off] == attr_lfn) {
            lf.priv_lfn_add(s, ent);
            continue;
        }
        var lnunits: u32 = 0;
        const lunits = lf.priv_lfn_units_for(s, ent, &lnunits);
        if (lunits != null and utf.priv_utf16_ieq(needle, nneedle, lunits.?, lnunits) != 0) {
            out_lba.* = cur_lba;
            out_entry_off.* = e * @as(u32, @intCast(dirent_len));
            @memcpy(out_entry[0..dirent_len], ent[0..dirent_len]);
            return .found;
        }
        lf.priv_lfn_reset(s); // 8.3 entry consumed -> next chain starts fresh
    }
    return .more;
}

/// Find a file's 8.3 entry by its long name: walk the directory named by
/// `loc`, reassembling each LFN chain and matching it against `want`.
///
/// `want` is a NUL-terminated path leaf; a leading slash is ignored (flat
/// root). On match, `out_lba` / `out_entry_off` / `out_entry` identify the
/// on-disk 8.3 entry. A name longer than any chain this format stores simply
/// never matches. Not thread-safe; the caller serialises directory access.
pub export fn priv_dir_find_long(
    m: [*c]const Mount,
    loc: [*c]const c.dir_loc_t,
    want: [*c]const u8,
    out_lba: [*c]u64,
    out_entry_off: [*c]u32,
    out_entry: [*c]u8,
) callconv(.c) u16 {
    var want_leaf = want;
    if (want_leaf[0] == '/') want_leaf += 1; // flat root: ignore a leading slash
    var needle: [lf.write_max]u16 = @splat(0);
    var nneedle: u32 = 0;
    const nerr = utf.priv_utf8_to_utf16(@ptrCast(want_leaf), &needle, lf.write_max, &nneedle);
    if (nerr == c.k_ra8_err_no_mem) {
        // Longer than any long name this format stores, so nothing here is it.
        return err_not_found;
    }
    if (nerr != ok) {
        return nerr;
    }
    var w = std.mem.zeroes(c.dir_walk_t);
    c.priv_dir_walk_init_loc(m, loc, &w);
    var s: LfnState = std.mem.zeroes(LfnState);
    lf.priv_lfn_reset(&s);
    var eod: u8 = 0;
    const buf: [*]u8 = c.priv_sec_walk();
    while (eod == 0) {
        var err = c.priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) {
            return err;
        }
        const scan = scanSector(m, &needle, nneedle, buf, w.cur_lba, &s, out_lba, out_entry_off, out_entry);
        if (scan == .found) {
            return ok;
        }
        if (scan == .eod) {
            return err_not_found;
        }
        err = c.priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) {
            return err;
        }
    }
    return err_not_found;
}

/// Look up a directory entry by either of its two on-disk names: the caller
/// writes one leaf, and this finds the entry whether it is filed 8.3 under
/// its own short name or as a `~N` alias whose long-name chain spells the
/// leaf. 8.3-representable names try the cheap direct scan first; anything
/// else (or a miss, which also catches a file filed under a generated alias)
/// reassembles long names and compares those, so `Long Name.txt` opens by
/// the name the caller wrote even when its disk entry says `LONGNA~1.TXT`.
/// Not thread-safe; the caller serialises directory access.
pub export fn priv_dir_lookup_any(
    m: [*c]const Mount,
    loc: [*c]const c.dir_loc_t,
    leaf: [*c]const u8,
    out_lba: [*c]u64,
    out_entry_off: [*c]u32,
    out_entry: [*c]u8,
) callconv(.c) u16 {
    var name83: [c.k_max_8_3_name]u8 = @splat(' ');
    var err: u16 = err_not_found;
    if (lf.priv_path_to_83(leaf, &name83) != 0) {
        err = c.priv_dir_find(m, loc, &name83, out_lba, out_entry_off, out_entry);
    }
    if (err == err_not_found) {
        // A name that is not 8.3-representable, or one whose 8.3 lookup
        // missed because the file is filed under a generated `~N` alias,
        // can still match the reassembled long name.
        err = priv_dir_find_long(m, loc, leaf, out_lba, out_entry_off, out_entry);
    }
    return err;
}

// ------------------------------------------------------- the 8.3 find

extern fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void;
extern fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16;
extern fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_sec_walk() callconv(.c) [*c]u8;
extern fn priv_byte_equal(a: [*c]const u8, b: [*c]const u8, n: u32) callconv(.c) u8;
extern fn priv_byte_copy(dst: [*c]u8, src: [*c]const u8, n: u32) callconv(.c) void;

const ebytes: u32 = @intCast(dirent_len);
const name_len: u32 = @intCast(c.k_dir_name_field_len);

/// The port of `priv_dir_find`: scan the directory `loc` names for the first
/// live 8.3 entry whose name field equals `name83` byte for byte, reporting
/// its sector, byte offset and full 32-byte image. Deleted (0xE5) and LFN
/// (attr 0x0F) slots are skipped, the end-of-directory marker (0x00) stops
/// the scan early, and the walk running off the directory answers not_found.
pub export fn priv_dir_find(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, name83: [*c]const u8, out_lba: [*c]u64, out_entry_off: [*c]u32, out_entry: [*c]u8) callconv(.c) u16 {
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    priv_dir_walk_init_loc(m, loc, &w);
    var eod: u8 = 0;
    const buf = priv_sec_walk();
    while (eod == 0) {
        var err = priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) return err;
        var e: u32 = 0;
        while (e < priv_dir_eps(m)) : (e += 1) {
            const ent = buf + e * ebytes;
            if (ent[name_off] == marker_eod) return err_not_found;
            if (ent[name_off] == marker_deleted) continue;
            if (ent[attr_off] == attr_lfn) continue;
            if (priv_byte_equal(ent, name83, name_len) != 0) {
                out_lba.* = w.cur_lba;
                out_entry_off.* = e * ebytes;
                priv_byte_copy(out_entry, ent, ebytes);
                return ok;
            }
        }
        err = priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) return err;
    }
    return err_not_found;
}
