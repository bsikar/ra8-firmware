//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! priv_free_chain (RA8FW-757) against the walker fakes' 32-entry FAT.

const std = @import("std");
const fs = @import("ra8_fs");
const fc = fs.free_chain;
const fake = @import("fs_walker_fake.zig");

const fat32: u8 = fs.c.k_ra8_fs_type_fat32;
const eoc: u32 = 0x0FFF_FFFF;

fn setup(clusters: u32) void {
    fake.reset(fat32);
    fake.mount_store.count_of_clusters = clusters;
}

test "frees a three-cluster chain and credits each one" {
    setup(20);
    fake.fat[5] = 9;
    fake.fat[9] = 4;
    fake.fat[4] = eoc;
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 0), fake.fat[5]);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[9]);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[4]);
    try std.testing.expectEqual(@as(u32, 3), fake.gave);
    try std.testing.expectEqual(@as(u32, 4), fake.hint_low);
}

test "start outside the data region frees nothing" {
    setup(20);
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 0));
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 1));
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 22));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_reads);
    try std.testing.expectEqual(@as(u32, 0), fake.gave);
}

test "a link past the data region ends the walk after that cluster" {
    setup(10);
    fake.fat[3] = 30;
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 3));
    try std.testing.expectEqual(@as(u32, 1), fake.fat_sets);
    try std.testing.expectEqual(@as(u32, 1), fake.gave);
}

test "a chain that loops back trips the guard" {
    setup(4);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 5;
    fake.fat[5] = 2;
    // Clusters 2..5 are freed, the loop returns to 2 (now free), and the
    // fifth step outruns the four-cluster volume, which only a cycle can do.
    try std.testing.expectEqual(fc.err_protocol, fc.priv_free_chain(fake.mount(), 2));
    try std.testing.expectEqual(@as(u32, 5), fake.gave);
    try std.testing.expectEqual(@as(u32, 2), fake.hint_low);
}

test "read error stops before anything is freed" {
    setup(20);
    fake.fat[5] = eoc;
    fake.fat_err = fs.c.k_ra8_err_hw_error;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_hw_error), fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_sets);
    try std.testing.expectEqual(@as(u32, 0), fake.gave);
}

test "write error is returned and the cluster is not credited" {
    setup(20);
    fake.fat[5] = eoc;
    fake.fat_set_err = fs.c.k_ra8_err_hw_error;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_hw_error), fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 0), fake.gave);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), fake.hint_low);
}
