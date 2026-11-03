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

test "a FAT12/16 fixed root refuses to grow" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    try std.testing.expectEqual(err_no_mem, write.internal_dir_grow(m, &loc));
}

test "grow appends one zeroed cluster at the chain's end" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    try std.testing.expectEqual(ok, write.internal_dir_grow(m, &loc));
    // The zero write and the FAT link write both ran; sector_written holds the last one (the FAT write).
    try std.testing.expect(fake.sector_written != null);
    var linked = false;
    for (fake.fat) |v| {
        if (v == 6) linked = true;
    }
    try std.testing.expect(linked);
}

test "a failed FAT link leaves the returned error intact" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    fake.fat_set_err = err_no_mem;
    try std.testing.expectEqual(err_no_mem, write.internal_dir_grow(m, &loc));
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}

test "LFN legality refuses the control range, DEL, and the FAT illegal set" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    try std.testing.expectEqual(@as(u8, 0), legal(0x00));
    try std.testing.expectEqual(@as(u8, 0), legal(0x1F));
    try std.testing.expectEqual(@as(u8, 0), legal(0x7F));
    for ([_]u16{ '"', '*', '/', ':', '<', '>', '?', '\\', '|' }) |ch| {
        try std.testing.expectEqual(@as(u8, 0), legal(ch));
    }
}

test "LFN legality allows the characters VFAT deliberately allows" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    for ([_]u16{ ' ', '+', ',', ';', '=', '[', ']' }) |ch| {
        try std.testing.expectEqual(@as(u8, 1), legal(ch));
    }
}

test "LFN legality allows ordinary and non-ASCII units" {
    const legal = fs.lfn.internal_unit_is_lfn_legal;
    try std.testing.expectEqual(@as(u8, 1), legal('a'));
    try std.testing.expectEqual(@as(u8, 1), legal('A'));
    try std.testing.expectEqual(@as(u8, 1), legal('0'));
    try std.testing.expectEqual(@as(u8, 1), legal(0xE9));
}

test "83 legality refuses the short-name exclusions and the ASCII cut" {
    const legal = fs.lfn.internal_unit_is_83_legal;
    for ([_]u16{ '.', '+', ',', ';', '=', '[', ']', ' ', 0xE9 }) |ch| {
        try std.testing.expectEqual(@as(u8, 0), legal(ch));
    }
}

test "83 legality allows ordinary name characters and pins the DEL fall-through" {
    const legal = fs.lfn.internal_unit_is_83_legal;
    for ([_]u16{ 'a', 'A', '0', '9', '_', '-' }) |ch| {
        try std.testing.expectEqual(@as(u8, 1), legal(ch));
    }
    // DEL is excluded upstream by the LFN check (the documented precondition);
    // the port preserves the C fall-through so the two never disagree.
    try std.testing.expectEqual(@as(u8, 1), legal(0x7F));
}
