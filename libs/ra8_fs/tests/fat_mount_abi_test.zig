//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Direct tests for the FAT geometry and partition-selection port: BPB
//! sector-size validation, mount geometry derivation with the FAT-type
//! thresholds, and MBR primary-entry selection.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const geo = fs.fat_mount_abi;

test "internal_bps_valid accepts powers of two inside the range" {
    try std.testing.expectEqual(@as(u8, 1), geo.internal_bps_valid(512));
    try std.testing.expectEqual(@as(u8, 1), geo.internal_bps_valid(1024));
    try std.testing.expectEqual(@as(u8, 1), geo.internal_bps_valid(4096));
    try std.testing.expectEqual(@as(u8, 0), geo.internal_bps_valid(511));
    try std.testing.expectEqual(@as(u8, 0), geo.internal_bps_valid(1536));
    try std.testing.expectEqual(@as(u8, 0), geo.internal_bps_valid(8192));
    try std.testing.expectEqual(@as(u8, 0), geo.internal_bps_valid(0));
}

test "internal_compute_geometry derives regions and the FAT type" {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.bytes_per_sector = 512;
    m.reserved_sectors = 32;
    m.num_fats = 2;
    m.fat_size_sectors = 100;
    m.root_entries = 512; // 512 * 32 = 16384 bytes = 32 sectors.
    m.sectors_per_cluster = 4;
    m.total_sectors = 20000;
    try std.testing.expectEqual(c.k_ra8_ok, geo.internal_compute_geometry(&m));
    try std.testing.expectEqual(@as(u64, 32), m.first_fat_lba);
    try std.testing.expectEqual(@as(u64, 32 + 2 * 100), m.first_root_lba);
    try std.testing.expectEqual(@as(u64, 32 + 2 * 100 + 32), m.first_data_lba);
    try std.testing.expectEqual(@as(u32, (20000 - 264) / 4), m.count_of_clusters);
    try std.testing.expectEqual(c.k_ra8_fs_type_fat16, m.type);
}

test "internal_compute_geometry refuses a medium that ends too early" {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.bytes_per_sector = 512;
    m.reserved_sectors = 32;
    m.num_fats = 2;
    m.fat_size_sectors = 100;
    m.sectors_per_cluster = 4;
    m.total_sectors = 200;
    try std.testing.expectEqual(c.k_ra8_err_validation_failed, geo.internal_compute_geometry(&m));
}

test "internal_mbr_select_entry reads primary entries" {
    var buf: [512]u8 = @splat(0);
    buf[510] = 0x55;
    buf[511] = 0xAA;
    buf[0x1C2] = 0x0B;
    buf[0x1C6] = 0x40;
    buf[0x1C2 + 16] = 0x0C;
    buf[0x1C6 + 16] = 0x02;
    var base: u64 = 0;
    try std.testing.expectEqual(c.k_ra8_ok, geo.internal_mbr_select_entry(&buf, 0, &base));
    try std.testing.expectEqual(@as(u64, 64), base);
    try std.testing.expectEqual(c.k_ra8_ok, geo.internal_mbr_select_entry(&buf, 1, &base));
    try std.testing.expectEqual(@as(u64, 2), base);
    try std.testing.expectEqual(c.k_ra8_err_out_of_range, geo.internal_mbr_select_entry(&buf, 4, &base));
    buf[0x1C2 + 32] = 0;
    try std.testing.expectEqual(c.k_ra8_err_not_found, geo.internal_mbr_select_entry(&buf, 2, &base));
}

test "internal_locate_indexed dispatches on sector 0" {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    const scratch = &fs.fat_state.g_fs_scratch;
    @memset(scratch, 0);
    var base: u64 = 0;
    try std.testing.expectEqual(c.k_ra8_err_not_found, geo.internal_locate_indexed(&m, 0, &base));
    scratch[510] = 0x55;
    scratch[511] = 0xAA;
    scratch[0x1C2] = 0x0B;
    scratch[0x1C6] = 0x80;
    try std.testing.expectEqual(c.k_ra8_ok, geo.internal_locate_indexed(&m, 0, &base));
    try std.testing.expectEqual(@as(u64, 0x80), base);
}
