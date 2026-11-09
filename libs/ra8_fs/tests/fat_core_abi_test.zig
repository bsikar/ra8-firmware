//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `ra8_fs_fat_core_abi.zig`: the little-endian 32/64-bit
//! writers, the byte-compare, the end-of-chain write selector, and the
//! free-cluster allocator's scan order (hint clamp, skip, wrap, full-volume
//! exhaustion). The FAT entry reader is the walker fake's array-backed stub.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");

const ok: u16 = 0;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const first_data: u32 = 2;

fn mount4() *c.ra8_fs_mount_t {
    fake.reset(c.k_ra8_fs_type_fat16);
    const m = @constCast(fake.mount());
    fs.alloc.priv_alloc_state_bind(m);
    m.count_of_clusters = 4;
    fake.fat_err = 0;
    return m;
}

test "alloc takes the first free cluster and moves the hint" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, first_data);
    fake.fat[2] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 2), out);
    try std.testing.expectEqual(@as(u32, 3), fs.alloc.priv_alloc_hint_get(m));
}

test "alloc skips used clusters" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, first_data);
    fake.fat[2] = 3;
    fake.fat[3] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 3), out);
}

test "alloc wraps once past the end of the data region" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, first_data);
    fake.fat[2] = 1;
    fake.fat[3] = 1;
    fake.fat[4] = 1;
    fake.fat[5] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 5), out);
}

test "alloc restarts at cluster 2 when the hint sits past the end" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, 100);
    fake.fat[2] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 2), out);
}

test "alloc accepts a hint at the last data cluster" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, 5);
    fake.fat[5] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 5), out);
}

test "alloc takes the next cluster when the hint is exactly past the end" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, 6);
    fake.fat[2] = 1;
    fake.fat[3] = 0;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fs.fat_core.priv_alloc_cluster(m, &out));
    try std.testing.expectEqual(@as(u32, 3), out);
}

test "alloc reports no memory on a full volume" {
    const m = mount4();
    fs.alloc.priv_alloc_hint_set(m, first_data);
    fake.fat[2] = 1;
    fake.fat[3] = 1;
    fake.fat[4] = 1;
    fake.fat[5] = 1;
    var out: u32 = 0;
    try std.testing.expectEqual(err_no_mem, fs.fat_core.priv_alloc_cluster(m, &out));
}

test "the EOC write value follows the FAT type" {
    fake.reset(c.k_ra8_fs_type_fat12);
    try std.testing.expectEqual(@as(u32, 0x0FFF), fs.fat_core.priv_eoc_write(fake.mount()));
    fake.reset(c.k_ra8_fs_type_fat16);
    try std.testing.expectEqual(@as(u32, 0xFFFF), fs.fat_core.priv_eoc_write(fake.mount()));
    fake.reset(c.k_ra8_fs_type_fat32);
    try std.testing.expectEqual(@as(u32, 0x0FFFFFFF), fs.fat_core.priv_eoc_write(fake.mount()));
    fake.reset(c.k_ra8_fs_type_exfat);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), fs.fat_core.priv_eoc_write(fake.mount()));
}

test "wr32 and wr64 write little-endian fields" {
    var buf: [12]u8 = @splat(0);
    fs.fat_core.priv_wr32(&buf, 0xDEADBEEF);
    try std.testing.expectEqual([4]u8{ 0xEF, 0xBE, 0xAD, 0xDE }, buf[0..4].*);
    fs.fat_core.priv_wr64(&buf, 0x0123456789ABCDEF);
    try std.testing.expectEqual([8]u8{ 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 }, buf[0..8].*);
}

test "byte_equal compares n bytes and treats an empty range as equal" {
    try std.testing.expectEqual(@as(u8, 1), fs.fat_core.priv_byte_equal("abc", "abc", 3));
    try std.testing.expectEqual(@as(u8, 0), fs.fat_core.priv_byte_equal("abc", "abd", 3));
    try std.testing.expectEqual(@as(u8, 1), fs.fat_core.priv_byte_equal("abc", "xyz", 0));
}
