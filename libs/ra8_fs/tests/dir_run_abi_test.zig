//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the free-slot run finder (`ra8_fs_dir_run_abi.zig`):
//! runs over deleted and end-of-directory markers, the restart after a live
//! entry, first-fit starts, the slot wall at the end of the laid directory,
//! backend read errors, and the no-memory answer when no run exists.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const run = fs.dir_run;

const ok: u16 = c.k_ra8_ok;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const marker_used: u8 = @intCast(c.k_dir_marker_free_used);
const marker_eod: u8 = @intCast(c.k_dir_marker_free_perm);

fn setup(fat_type: u8) *const c.ra8_fs_mount_t {
    fake.reset(fat_type);
    fake.dir_lba = fake.walk_lba;
    fake.walk_fixed = 1;
    fake.eps = 16;
    for (&fake.dir) |*e| e.* = @splat(0x85);
    return fake.mount();
}

fn setLive(i: usize) void {
    fake.dir[i][0] = 'A';
}
fn setDeleted(i: usize) void {
    fake.dir[i][0] = marker_used;
}
fn setEod(i: usize) void {
    fake.dir[i][0] = marker_eod;
}

test "finds a run of deleted slots at exact length" {
    const m = setup(c.k_ra8_fs_type_fat16);
    setLive(0);
    setLive(1);
    setDeleted(2);
    setDeleted(3);
    setLive(4);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(ok, run.priv_dir_find_free_run(m, &loc, 2, &slot));
    try std.testing.expectEqual(@as(u32, 2), slot.ent);
    try std.testing.expectEqual(@as(u64, fake.dir_lba), slot.w.cur_lba);
}

test "need 1 answers the first fit" {
    const m = setup(c.k_ra8_fs_type_fat16);
    setLive(0);
    setLive(1);
    setDeleted(2);
    setDeleted(3);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(ok, run.priv_dir_find_free_run(m, &loc, 1, &slot));
    try std.testing.expectEqual(@as(u32, 2), slot.ent);
}

test "restarts the run after a live entry, 0x00 counts as free" {
    const m = setup(c.k_ra8_fs_type_fat16);
    setDeleted(0);
    setLive(1);
    setEod(2);
    setDeleted(3);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(ok, run.priv_dir_find_free_run(m, &loc, 2, &slot));
    try std.testing.expectEqual(@as(u32, 2), slot.ent);
}

test "short run followed by a longer one answers the longer start" {
    const m = setup(c.k_ra8_fs_type_fat16);
    setLive(0);
    setDeleted(1);
    setLive(2);
    setDeleted(3);
    setDeleted(4);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(ok, run.priv_dir_find_free_run(m, &loc, 2, &slot));
    try std.testing.expectEqual(@as(u32, 3), slot.ent);
}

test "run that meets the wall of live slots is too short" {
    const m = setup(c.k_ra8_fs_type_fat16);
    var i: usize = 0;
    while (i < 6) : (i += 1) setLive(i);
    setDeleted(6);
    setDeleted(7);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(ok, run.priv_dir_find_free_run(m, &loc, 2, &slot));
    try std.testing.expectEqual(@as(u32, 6), slot.ent);
    try std.testing.expectEqual(err_no_mem, run.priv_dir_find_free_run(m, &loc, 3, &slot));
}

test "full directory answers no_mem" {
    const m = setup(c.k_ra8_fs_type_fat16);
    var i: usize = 0;
    while (i < fake.slots) : (i += 1) setLive(i);
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(err_no_mem, run.priv_dir_find_free_run(m, &loc, 1, &slot));
}

test "backend read error propagates" {
    const m = setup(c.k_ra8_fs_type_fat16);
    fake.sector_read_err = c.k_ra8_err_invalid_state;
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var slot: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_invalid_state), run.priv_dir_find_free_run(m, &loc, 1, &slot));
}
