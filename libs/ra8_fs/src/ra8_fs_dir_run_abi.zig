//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Free-slot run finder for FAT directory space: the port of the slice of
//! `ra8_fs_fat_lfn_write.c` that locates `need` consecutive writable 32-byte
//! directory slots. 0xE5 (deleted) and 0x00 (end-of-directory) both count as
//! free; a run that starts at 0x00 stays terminated because the slot after
//! the run keeps its own 0x00.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;
const off_name: usize = @intCast(c.k_dir_off_name);
const marker_free_perm: u8 = @intCast(c.k_dir_marker_free_perm);
const marker_free_used: u8 = @intCast(c.k_dir_marker_free_used);

extern fn priv_dir_walk_init_loc(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, w: [*c]c.dir_walk_t) callconv(.c) void;
extern fn priv_dir_walk_next_sector(m: [*c]const c.ra8_fs_mount_t, w: [*c]c.dir_walk_t, out_eod: [*c]u8) callconv(.c) u16;
extern fn priv_read_sector(m: [*c]const c.ra8_fs_mount_t, lba: u64, buf: [*c]u8) callconv(.c) u16;
extern fn priv_dir_eps(m: [*c]const c.ra8_fs_mount_t) callconv(.c) u32;
extern fn priv_sec_walk() callconv(.c) [*c]u8;

/// A slot is writable when its first byte is a free marker: 0xE5 (deleted)
/// or 0x00 (end-of-directory, past which every allocated slot is unused).
pub export fn internal_slot_is_free(ent: [*c]const u8) callconv(.c) u8 {
    if (ent[off_name] == marker_free_perm or ent[off_name] == marker_free_used) {
        return 1;
    }
    return 0;
}

/// Find the first run of at least `need` consecutive free slots in the
/// directory `loc` names, writing its start (sector cursor plus entry index)
/// to `out`. Walks sector by sector; answers no_mem when the directory holds
/// no such run.
pub export fn priv_dir_find_free_run(m: [*c]const c.ra8_fs_mount_t, loc: [*c]const c.dir_loc_t, need: u32, out: [*c]c.dir_slot_t) callconv(.c) u16 {
    var w: c.dir_walk_t = std.mem.zeroes(c.dir_walk_t);
    priv_dir_walk_init_loc(m, loc, &w);
    var start: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var run: u32 = 0;
    var eod: u8 = 0;
    const buf = priv_sec_walk();
    while (eod == 0) {
        var err = priv_read_sector(m, w.cur_lba, buf);
        if (err != ok) return err;
        var e: u32 = 0;
        while (e < priv_dir_eps(m)) : (e += 1) {
            if (internal_slot_is_free(buf + e * entry_bytes) == 0) {
                run = 0;
                continue;
            }
            if (run == 0) {
                start.w = w;
                start.ent = e;
            }
            run += 1;
            if (run >= need) {
                out.* = start;
                return ok;
            }
        }
        err = priv_dir_walk_next_sector(m, &w, &eod);
        if (err != ok) return err;
    }
    return err_no_mem;
}
