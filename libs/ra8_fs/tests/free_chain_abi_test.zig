//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! priv_free_chain (RA8FW-757) against the walker fakes' 32-entry FAT.

const std = @import("std");
const fs = @import("ra8_fs");
const fc = fs.free_chain;
const al = fs.alloc;
const fake = @import("fs_walker_fake.zig");

const fat32: u8 = fs.c.k_ra8_fs_type_fat32;
const eoc: u32 = 0x0FFF_FFFF;

fn setup(clusters: u32) void {
    fake.reset(fat32);
    fake.mount_store.count_of_clusters = clusters;
}

test "frees a three-cluster chain and credits each one" {
    setup(20);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 10);
    al.priv_alloc_hint_set(fake.mount(), 100);
    fake.fat[5] = 9;
    fake.fat[9] = 4;
    fake.fat[4] = eoc;
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 0), fake.fat[5]);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[9]);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[4]);
    try std.testing.expectEqual(@as(u32, 13), al.priv_free_count_peek(fake.mount()));
    try std.testing.expectEqual(@as(u32, 4), al.priv_alloc_hint_get(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

test "start outside the data region frees nothing" {
    setup(20);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 7);
    al.priv_alloc_hint_set(fake.mount(), 100);
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 0));
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 1));
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 22));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_reads);
    try std.testing.expectEqual(@as(u32, 7), al.priv_free_count_peek(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

test "a link past the data region ends the walk after that cluster" {
    setup(10);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 5);
    al.priv_alloc_hint_set(fake.mount(), 100);
    fake.fat[3] = 30;
    try std.testing.expectEqual(fc.ok, fc.priv_free_chain(fake.mount(), 3));
    try std.testing.expectEqual(@as(u32, 1), fake.fat_sets);
    try std.testing.expectEqual(@as(u32, 6), al.priv_free_count_peek(fake.mount()));
    try std.testing.expectEqual(@as(u32, 3), al.priv_alloc_hint_get(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

test "a chain that loops back trips the guard" {
    setup(4);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 0);
    al.priv_alloc_hint_set(fake.mount(), 100);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 5;
    fake.fat[5] = 2;
    // Clusters 2..5 are freed, the loop returns to 2 (now free), and the
    // fifth step outruns the four-cluster volume, which only a cycle can do.
    try std.testing.expectEqual(fc.err_protocol, fc.priv_free_chain(fake.mount(), 2));
    // Five credits on a four-cluster volume: the real counter clamps.
    try std.testing.expectEqual(@as(u32, 4), al.priv_free_count_peek(fake.mount()));
    try std.testing.expectEqual(@as(u32, 2), al.priv_alloc_hint_get(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

test "read error stops before anything is freed" {
    setup(20);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 7);
    fake.fat[5] = eoc;
    fake.fat_err = fs.c.k_ra8_err_hw_error;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_hw_error), fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 0), fake.fat_sets);
    try std.testing.expectEqual(@as(u32, 7), al.priv_free_count_peek(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

test "write error is returned and the cluster is not credited" {
    setup(20);
    al.priv_alloc_state_bind(fake.mount());
    al.priv_free_count_cache(fake.mount(), 7);
    al.priv_alloc_hint_set(fake.mount(), 100);
    fake.fat[5] = eoc;
    fake.fat_set_err = fs.c.k_ra8_err_hw_error;
    try std.testing.expectEqual(@as(u16, fs.c.k_ra8_err_hw_error), fc.priv_free_chain(fake.mount(), 5));
    try std.testing.expectEqual(@as(u32, 7), al.priv_free_count_peek(fake.mount()));
    try std.testing.expectEqual(@as(u32, 100), al.priv_alloc_hint_get(fake.mount()));
    al.priv_alloc_state_release(fake.mount());
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}
