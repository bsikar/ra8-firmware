//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_fs_free_space on FAT and exFAT (RA8FW-740), against the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const sp = fs.space;
const fake = @import("fs_walker_fake.zig");

const fat16: u8 = fs.c.k_ra8_fs_type_fat16;
const exfat: u8 = fs.c.k_ra8_fs_type_exfat;

fn query(out: *sp.Space) u16 {
    out.* = std.mem.zeroes(sp.Space);
    return sp.ra8_fs_free_space(fake.mount(), out);
}

test "argument checks: null handle, null out, unmounted" {
    fake.reset(fat16);
    var s = std.mem.zeroes(sp.Space);
    try std.testing.expectEqual(sp.err_null_ptr, sp.ra8_fs_free_space(null, &s));
    try std.testing.expectEqual(sp.err_null_ptr, sp.ra8_fs_free_space(fake.mount(), null));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(sp.err_invalid_state, query(&s));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_reads);
}

test "FAT scans every data cluster, fills the totals and caches the count" {
    fake.reset(fat16);
    fake.mount_store.count_of_clusters = 10;
    for (2..12) |k| fake.fat[k] = 0xFFFF;
    fake.fat[4] = 0;
    fake.fat[9] = 0;
    fake.fat[11] = 0;
    fake.fat[12] = 0; // past the last cluster: never read
    var s: sp.Space = undefined;
    try std.testing.expectEqual(sp.ok, query(&s));
    try std.testing.expectEqual(@as(u32, 10), fake.fat_reads);
    try std.testing.expectEqual(@as(u32, 3), s.free_clusters);
    try std.testing.expectEqual(@as(u32, 7), s.used_clusters);
    try std.testing.expectEqual(@as(u32, 10), s.total_clusters);
    try std.testing.expectEqual(@as(u32, 4096), s.bytes_per_cluster);
    try std.testing.expectEqual(@as(u64, 40960), s.total_bytes);
    try std.testing.expectEqual(@as(u64, 12288), s.free_bytes);
    try std.testing.expectEqual(@as(u64, 28672), s.used_bytes);
    try std.testing.expectEqual(@as(u32, 3), fake.free_cached);
}

test "FAT uses the cached count without scanning" {
    fake.reset(fat16);
    fake.mount_store.count_of_clusters = 10;
    fake.free_cached = 6;
    var s: sp.Space = undefined;
    try std.testing.expectEqual(sp.ok, query(&s));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_reads);
    try std.testing.expectEqual(@as(u32, 6), s.free_clusters);
    try std.testing.expectEqual(@as(u32, 4), s.used_clusters);
}

test "FAT read failure propagates and leaves the cache unknown" {
    fake.reset(fat16);
    fake.mount_store.count_of_clusters = 10;
    fake.fat_err = 0x204;
    var s: sp.Space = undefined;
    try std.testing.expectEqual(@as(u16, 0x204), query(&s));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), fake.free_cached);
}

test "exFAT popcounts the bitmap across sectors and masks the tail byte" {
    fake.reset(exfat);
    fake.mount_store.count_of_clusters = 4100; // 512 full bytes + 4 tail bits
    fake.bitmap[0] = 0xFF;
    fake.bitmap[511] = 0x0F;
    fake.bitmap[512] = 0xF5; // only the low 4 bits count: 0b0101 -> 2
    fake.bitmap[513] = 0xFF; // past the end: never read
    var s: sp.Space = undefined;
    try std.testing.expectEqual(sp.ok, query(&s));
    try std.testing.expectEqual(@as(u32, 2), fake.io_reads);
    try std.testing.expectEqual(@as(u32, 14), s.used_clusters);
    try std.testing.expectEqual(@as(u32, 4086), s.free_clusters);
    try std.testing.expectEqual(@as(u32, 0), fake.fat_reads);
}

test "exFAT errors propagate" {
    fake.reset(exfat);
    fake.mount_store.count_of_clusters = 16;
    fake.bitmap_err = fs.c.k_ra8_err_not_found;
    var s: sp.Space = undefined;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_not_found), query(&s));
    fake.reset(exfat);
    fake.mount_store.count_of_clusters = 16;
    fake.sector_read_err = 0x205;
    try std.testing.expectEqual(@as(u16, 0x205), query(&s));
}
