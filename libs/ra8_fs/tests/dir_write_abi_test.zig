//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the directory write-path helpers
//! (`ra8_fs_dir_write_abi.zig`): alias probing stops at the first basis
//! with no directory entry, propagates backend errors, and honours the
//! Rule 2 tail bound; the slot cursor advances inside a sector without
//! touching the disk and rolls over with one write-back, one walk step,
//! and one read, answering no_mem when the walk runs off the directory.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const write = fs.dir_write;

const ok: u16 = c.k_ra8_ok;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_protocol: u16 = c.k_ra8_err_protocol_error;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;

fn setup() *const c.ra8_fs_mount_t {
    fake.reset(c.k_ra8_fs_type_fat16);
    fake.dir_lba = fake.walk_lba;
    fake.walk_fixed = 2;
    return fake.mount();
}

test "alias probing stops at the first basis with no entry" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var out11: [entry_bytes]u8 = @splat(0);
    fake.find83_not_found = true;
    try std.testing.expectEqual(ok, write.internal_alias_unique(m, &loc, null, 4, &out11));
}

test "alias probing propagates a backend error" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    var out11: [entry_bytes]u8 = @splat(0);
    fake.find83_err = err_protocol;
    try std.testing.expectEqual(err_protocol, write.internal_alias_unique(m, &loc, null, 4, &out11));
}

test "the slot cursor advances inside the sector without touching the disk" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    fake.sector_written = null;
    cur.ent = 5;
    try std.testing.expectEqual(ok, write.internal_slot_advance(m, &cur, &buf, &lba));
    try std.testing.expectEqual(@as(u32, 6), cur.ent);
    try std.testing.expect(fake.sector_written == null);
}

test "the slot cursor rolls over with one write, one walk step, one read" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    fake.sector_written = null;
    cur.ent = 15;
    cur.w.fixed_remaining = 2;
    cur.w.cur_lba = fake.dir_lba;
    try std.testing.expectEqual(ok, write.internal_slot_advance(m, &cur, &buf, &lba));
    try std.testing.expectEqual(@as(u32, 0), cur.ent);
    try std.testing.expectEqual(fake.dir_lba, fake.sector_written.?);
    try std.testing.expectEqual(fake.dir_lba + 1, lba);
}

test "a rollover off the directory's end answers no_mem" {
    const m = setup();
    var cur: c.dir_slot_t = std.mem.zeroes(c.dir_slot_t);
    var buf: [512]u8 = @splat(0);
    var lba: u64 = fake.dir_lba;
    cur.ent = 15;
    cur.w.fixed_remaining = 0;
    cur.w.cur_lba = fake.dir_lba;
    try std.testing.expectEqual(err_no_mem, write.internal_slot_advance(m, &cur, &buf, &lba));
}
