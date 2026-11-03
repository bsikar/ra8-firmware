//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the FAT entry layer: get/set across the four flavours, the
//! FAT12 sector-straddle packing, multi-copy mirroring and the error paths.
//! FAT-backed chains live in the walker fake's array, served to the block
//! backend sector by sector, so the real `priv_fat_get`/`priv_fat_set` run
//! against the same fake device every other module tests against.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fe = fs.fat_entry;
const fake = @import("fs_walker_fake.zig");

const bps: u32 = 512;
const eoc32: u32 = 0x0FFF_FFFF;
const eocex: u32 = 0xFFFF_FFFF;

fn mkFat(comptime t: u32) *c.ra8_fs_mount_t {
    fake.reset(@intCast(t));
    const mp = @constCast(fake.mount());
    mp.first_fat_lba = fake.fat_lba;
    mp.fat_size_sectors = fake.fat_secs;
    mp.num_fats = 1;
    return mp;
}

test "a FAT32 entry round-trips through the block backend" {
    const mp = mkFat(c.k_ra8_fs_type_fat32);
    fake.fat[5] = 7;
    var v: u32 = 0;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 5, &v));
    try std.testing.expectEqual(@as(u32, 7), v);
    // C setter contract: the value is masked to the cluster mask and the
    // reserved top bits come from the previous entry only, so an
    // out-of-range value masks down instead of saturating to EOC.
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 5, eoc32 + 1));
    try std.testing.expectEqual(@as(u32, 0), fake.fat[5]);
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 5, eoc32));
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 5, &v));
    try std.testing.expectEqual(@as(u32, eoc32), v);
    try std.testing.expectEqual(@as(u32, eoc32), fake.fat[5]);
}

test "a FAT32 set preserves the reserved top bits" {
    const mp = mkFat(c.k_ra8_fs_type_fat32);
    fake.fat[3] = 0xF000_00AA;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 3, 1));
    // The stored entry keeps the reserved top bits of the previous value.
    try std.testing.expectEqual(@as(u32, 0xF000_0001), fake.fat[3]);
    var v: u32 = 0;
    // The C getter masks the top 4 bits on read, so callers never see them.
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 3, &v));
    try std.testing.expectEqual(@as(u32, 1), v);
}

test "an exFAT entry keeps all 32 bits, end-of-chain included" {
    const mp = mkFat(c.k_ra8_fs_type_exfat);
    fake.fat[7] = eocex;
    var v: u32 = 0;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 7, &v));
    try std.testing.expectEqual(@as(u32, eocex), v);
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 7, eocex));
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 7, &v));
    try std.testing.expectEqual(@as(u32, eocex), v);
}

test "a FAT16 entry is two bytes wide" {
    const mp = mkFat(c.k_ra8_fs_type_fat16);
    fake.fat[9] = 0xFFF8;
    var v: u32 = 0;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 9, &v));
    try std.testing.expectEqual(@as(u32, 0xFFF8), v);
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 9, 0xFFF9));
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 9, &v));
    try std.testing.expectEqual(@as(u32, 0xFFF9), v);
}

test "a set mirrors into every FAT copy" {
    const mp = mkFat(c.k_ra8_fs_type_fat32);
    mp.num_fats = 2;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(mp, 5, 9));
    var v: u32 = 0;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(mp, 5, &v));
    try std.testing.expectEqual(@as(u32, 9), v);
    try std.testing.expectEqual(@as(u32, 2), fake.fat_sets);
}

test "a FAT12 entry packs and straddles the sector boundary" {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.type = c.k_ra8_fs_type_fat12;
    m.bytes_per_sector = bps;
    m.first_fat_lba = 10;
    m.fat_size_sectors = 2;
    m.num_fats = 1;
    const Wrap = struct {
        fn read(_: ?*anyopaque, lba: u64, _: u32, buf: [*c]u8) callconv(.c) u16 {
            const i: usize = @intCast(lba - 10);
            @memcpy(buf[0..bps], &disk[i]);
            return 0;
        }
        fn write(_: ?*anyopaque, lba: u64, _: u32, buf: [*c]const u8) callconv(.c) u16 {
            const i: usize = @intCast(lba - 10);
            @memcpy(&disk[i], buf[0..bps]);
            return 0;
        }
        var disk: [3][bps]u8 = @splat(@splat(0));
    };
    m.backend.read_block = Wrap.read;
    m.backend.write_block = Wrap.write;
    // Cluster 341 sits at byte offset 511: the last byte of the first FAT
    // sector, its second byte in the next one. Cluster 340 is even and stays
    // inside the sector, nibble-wise.
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(&m, 340, 0x123));
    try std.testing.expectEqual(@as(u8, 0x23), Wrap.disk[0][510]);
    try std.testing.expectEqual(@as(u8, 0x01), Wrap.disk[0][511]);
    try std.testing.expectEqual(fe.ok, fe.priv_fat_set(&m, 341, 0xABC));
    // The odd entry owns the high nibble only; the low nibble still holds
    // cluster 340's top bit (0x1 from its second byte) and C preserves it.
    try std.testing.expectEqual(@as(u8, 0xC1), Wrap.disk[0][511]);
    try std.testing.expectEqual(@as(u8, 0xAB), Wrap.disk[1][0]);
    var v: u32 = 0;
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(&m, 340, &v));
    try std.testing.expectEqual(@as(u32, 0x123), v);
    try std.testing.expectEqual(fe.ok, fe.priv_fat_get(&m, 341, &v));
    try std.testing.expectEqual(@as(u32, 0xABC), v);
}

test "cluster_to_lba maps the first data cluster onto first_data_lba" {
    const mp = mkFat(c.k_ra8_fs_type_fat32);
    mp.first_data_lba = 100;
    mp.sectors_per_cluster = 2;
    try std.testing.expectEqual(@as(u64, 100), fe.priv_cluster_to_lba(mp, 2));
    try std.testing.expectEqual(@as(u64, 106), fe.priv_cluster_to_lba(mp, 5));
}

test "a backend read error comes back from get" {
    const mp = mkFat(c.k_ra8_fs_type_fat32);
    fake.fat_err = c.k_ra8_err_hw_error;
    var v: u32 = 0;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_hw_error), fe.priv_fat_get(mp, 5, &v));
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}
