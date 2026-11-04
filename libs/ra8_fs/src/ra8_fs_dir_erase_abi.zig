//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Directory erase-chain helpers for FAT: the port of the static slice of
//! `ra8_fs_fat_lfn_write.c` that collects the long-name slots preceding one
//! 8.3 entry and marks every listed slot deleted. The remaining C wrapper
//! `priv_dir_erase_chain` links these exports.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_not_found: u16 = c.k_ra8_err_not_found;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;
const erase_max: u32 = @intCast(c.k_lfn_erase_max);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_name: usize = @intCast(c.k_dir_off_name);
const off_csum: usize = @intCast(c.k_lfn_off_checksum);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);

extern fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void;
extern fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16;
extern fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_write_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_sec_walk() callconv(.c) [*c]u8;

/// Append one slot position to the erase run, shifting the oldest out when
/// the run sits at `k_lfn_erase_max` (NASA Power of 10 Rule 2: bounded shift).
pub export fn internal_run_push(run: [*c]c.dir_pos_t, len_io: [*c]u32, lba: u64, off: u32) callconv(.c) void {
    var len: u32 = len_io.*;
    if (len == erase_max) {
        var i: u32 = 1;
        while (i < erase_max) : (i += 1) {
            run[i - 1] = run[i];
        }
        len -= 1;
    }
    run[len].lba = lba;
    run[len].off = off;
    len_io.* = len + 1;
}

/// Collect the long-name slots that belong to one 8.3 entry: walk the
/// directory to the entry at (`tlba`, `toff`), keeping the run of attr-0x0F
/// slots that immediately precede it and carry `csum`. A live entry, a
/// deleted slot, or a long-name slot with a different checksum resets the
/// run, so a stale chain in front of the file is not swept up with it.
pub export fn internal_dir_collect_chain(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, tlba: u64, toff: u32, csum: u8, run: [*c]c.dir_pos_t, out_len: [*c]u32) callconv(.c) u16 {
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    priv_dir_walk_init_loc(m, loc, &w);
    var len: u32 = 0;
    var eod: u8 = 0;
    const buf = priv_sec_walk();
    while (eod == 0) {
        var err = priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) return err;
        var e: u32 = 0;
        while (e < priv_dir_eps(m)) : (e += 1) {
            const off: u32 = e * entry_bytes;
            if (w.cur_lba == tlba and off == toff) {
                out_len.* = len;
                return ok;
            }
            const ent = buf + off;
            if (ent[off_attr] == attr_lfn and ent[off_name] != marker_free_used and ent[off_csum] == csum) {
                internal_run_push(run, &len, w.cur_lba, off);
            } else {
                len = 0;
            }
        }
        err = priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) return err;
    }
    return err_not_found;
}

/// Mark every listed slot deleted, one sector read-modify-write at a time.
/// The addresses arrive in increasing order, so one buffer is enough: it is
/// written back only when the next address moves to a different sector.
pub export fn internal_dir_erase_positions(m: [*c]const c.ra8_fs_mount_t, pos: [*c]const c.dir_pos_t, count: u32) callconv(.c) u16 {
    const buf = priv_sec_walk();
    var cur: u64 = pos[0].lba;
    var err = priv_read_sector(m, cur, buf);
    if (err != ok) return err;
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        if (pos[i].lba != cur) {
            err = priv_write_sector(m, cur, buf);
            if (err != ok) return err;
            cur = pos[i].lba;
            err = priv_read_sector(m, cur, buf);
            if (err != ok) return err;
        }
        buf[pos[i].off + off_name] = marker_free_used;
    }
    return priv_write_sector(m, cur, buf);
}

/// Erase one 8.3 directory entry plus the long-name chain filed ahead of it:
/// collect the chain whose slots carry this entry's name checksum, append
/// the entry's own slot, and mark every position deleted. Freeing the
/// entry's cluster chain stays the caller's job.
pub export fn priv_dir_erase_chain(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, lba: u64, off: u32, name83: [*c]const u8) callconv(.c) u16 {
    const lfn = @import("ra8_fs_lfn_abi.zig");
    var all: [erase_max + 1]c.dir_pos_t = std.mem.zeroes([erase_max + 1]c.dir_pos_t);
    var len: u32 = 0;
    const cerr = internal_dir_collect_chain(m, loc, lba, off, lfn.priv_sfn_checksum(name83), &all, &len);
    if (cerr != c.k_ra8_ok) return cerr;
    all[len].lba = lba;
    all[len].off = off;
    return internal_dir_erase_positions(m, &all, len + 1);
}
