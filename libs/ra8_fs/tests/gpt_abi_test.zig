//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GPT partition locators (RA8FW-742), against the walker fakes: the header
//! sits at LBA 1 and the entry array at LBA 2, four 128-byte entries per
//! 512-byte sector.

const std = @import("std");
const fs = @import("ra8_fs");
const gpt = fs.gpt;
const fake = @import("fs_walker_fake.zig");

fn header(entry_lba: u64, count: u32, size: u32) void {
    fake.reset(fs.c.k_ra8_fs_type_fat32);
    const h = &fake.disk[0];
    @memcpy(h[0..8], gpt.signature);
    std.mem.writeInt(u64, h[0x48..][0..8], entry_lba, .little);
    std.mem.writeInt(u32, h[0x50..][0..4], count, .little);
    std.mem.writeInt(u32, h[0x54..][0..4], size, .little);
}

fn entry(index: usize, guid: [16]u8, first: u64) void {
    const sec = &fake.disk[1 + index / 4];
    const e = sec[(index % 4) * 128 ..][0..128];
    @memcpy(e[0..16], &guid);
    std.mem.writeInt(u64, e[0x20..][0..8], first, .little);
}

const other_guid: [16]u8 = @splat(0x11);

fn volume() struct { u16, u64 } {
    var base: u64 = 0;
    const err = gpt.priv_gpt_locate_volume(fake.mount(), &base);
    return .{ err, base };
}

fn partition(index: u8) struct { u16, u64 } {
    var base: u64 = 0;
    const err = gpt.priv_gpt_locate_partition(fake.mount(), index, &base);
    return .{ err, base };
}

test "header checks: signature, entry LBA, entry size" {
    header(2, 6, 128);
    fake.disk[0][0] = 'X';
    try std.testing.expectEqual(gpt.err_validation, volume()[0]);
    header(0, 6, 128);
    try std.testing.expectEqual(gpt.err_validation, volume()[0]);
    header(2, 6, 256);
    try std.testing.expectEqual(gpt.err_not_supported, partition(0)[0]);
}

test "volume prefers Basic Data over an earlier used entry, across sectors" {
    header(2, 6, 128);
    entry(1, other_guid, 100);
    entry(5, gpt.basic_data_guid, 2048);
    try std.testing.expectEqual(.{ gpt.ok, @as(u64, 2048) }, volume());
}

test "volume falls back to the first used entry, skipping zero first-LBA" {
    header(2, 6, 128);
    entry(0, other_guid, 0);
    entry(2, other_guid, 300);
    entry(4, other_guid, 500);
    try std.testing.expectEqual(.{ gpt.ok, @as(u64, 300) }, volume());
}

test "volume with no used entry is not found" {
    header(2, 6, 128);
    try std.testing.expectEqual(gpt.err_not_found, volume()[0]);
}

test "partition selects by index and rejects empty, zero-LBA and out-of-range" {
    header(2, 6, 128);
    entry(1, other_guid, 0);
    entry(5, gpt.basic_data_guid, 2048);
    try std.testing.expectEqual(.{ gpt.ok, @as(u64, 2048) }, partition(5));
    try std.testing.expectEqual(gpt.err_not_found, partition(0)[0]);
    try std.testing.expectEqual(gpt.err_validation, partition(1)[0]);
    try std.testing.expectEqual(gpt.err_out_of_range, partition(6)[0]);
}

test "entry count is clamped to the scan bound, and read errors propagate" {
    header(2, 1000, 128);
    try std.testing.expectEqual(gpt.err_out_of_range, partition(200)[0]);
    header(2, 6, 128);
    fake.sector_read_err = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), volume()[0]);
    try std.testing.expectEqual(@as(u16, 0x204), partition(0)[0]);
}
