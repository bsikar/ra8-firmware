//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The primitive-layer ABI tests: little-endian field access, the
//! mount-geometry reads, byte fill/copy, the end-of-chain classifier, and the
//! sector wrappers that forward to the mount's block backend.

const std = @import("std");
const fs = @import("ra8_fs");
const prim = fs.prim;
const c = fs.c;
const fake = @import("fs_walker_fake.zig");

const ok: u16 = c.k_ra8_ok;

var seen_lba: u64 = 0;
var seen_count: u32 = 0;
var backend_err: u16 = 0;

fn read_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]u8) callconv(.c) u16 {
    _ = ctx;
    seen_lba = lba;
    seen_count = count;
    if (backend_err != 0) return backend_err;
    buf[0] = 0xAA;
    return ok;
}

fn write_block_backend(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.c) u16 {
    _ = ctx;
    seen_lba = lba;
    seen_count = count;
    _ = buf;
    if (backend_err != 0) return backend_err;
    return ok;
}

fn mount() c.ra8_fs_mount_t {
    var m = std.mem.zeroes(c.ra8_fs_mount_t);
    m.bytes_per_sector = 512;
    m.sectors_per_cluster = 4;
    m.type = c.k_ra8_fs_type_fat16;
    m.partition_base_lba = 100;
    m.backend.read_block = read_block_backend;
    m.backend.write_block = write_block_backend;
    return m;
}

test "little-endian field reads and the 16-bit writer round-trip" {
    var buf: [8]u8 = .{ 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    try std.testing.expectEqual(@as(u16, 0x2301), prim.priv_rd16(&buf));
    try std.testing.expectEqual(@as(u32, 0x67452301), prim.priv_rd32(&buf));
    try std.testing.expectEqual(@as(u64, 0xEFCDAB8967452301), prim.priv_rd64(&buf));
    var out: [2]u8 = @splat(0);
    prim.priv_wr16(&out, 0xBEEF);
    try std.testing.expectEqualSlices(u8, &.{ 0xEF, 0xBE }, &out);
}

test "geometry reads come from the mount fields" {
    const m = mount();
    try std.testing.expectEqual(@as(u32, 512), prim.priv_bps(&m));
    try std.testing.expectEqual(@as(u32, 2048), prim.priv_cluster_bytes(&m));
    try std.testing.expectEqual(@as(u32, 16), prim.priv_dir_eps(&m));
}

test "byte fill and byte copy" {
    var dst: [6]u8 = @splat(0);
    prim.priv_byte_fill(&dst, 0x5A, 6);
    try std.testing.expectEqualSlices(u8, &.{ 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A }, &dst);
    var other: [6]u8 = @splat(0x11);
    other[3] = 0x22;
    var dst2: [6]u8 = @splat(0);
    prim.priv_byte_copy(&dst2, &other, 6);
    try std.testing.expectEqualSlices(u8, &other, &dst2);
}

test "end-of-chain thresholds are per FAT type" {
    var m = mount();
    m.type = c.k_ra8_fs_type_fat12;
    try std.testing.expectEqual(@as(u8, 0), prim.priv_is_eoc(&m, 0x0FF7));
    try std.testing.expectEqual(@as(u8, 1), prim.priv_is_eoc(&m, 0x0FF8));
    m.type = c.k_ra8_fs_type_fat16;
    try std.testing.expectEqual(@as(u8, 0), prim.priv_is_eoc(&m, 0xFFF7));
    try std.testing.expectEqual(@as(u8, 1), prim.priv_is_eoc(&m, 0xFFFF));
    m.type = c.k_ra8_fs_type_fat32;
    try std.testing.expectEqual(@as(u8, 0), prim.priv_is_eoc(&m, 0x0FFFFFF7));
    try std.testing.expectEqual(@as(u8, 1), prim.priv_is_eoc(&m, 0x0FFFFFF8));
    m.type = c.k_ra8_fs_type_exfat;
    try std.testing.expectEqual(@as(u8, 0), prim.priv_is_eoc(&m, 0xFFFFFFF7));
    try std.testing.expectEqual(@as(u8, 1), prim.priv_is_eoc(&m, 0xFFFFFFFF));
}

test "sector read forwards to the backend with the partition base added" {
    seen_lba = 0;
    seen_count = 0;
    backend_err = 0;
    fake.reset(c.k_ra8_fs_type_fat16);
    const mp = @constCast(fake.mount());
    mp.*.partition_base_lba = 100;
    mp.*.backend.read_block = read_block_backend;
    mp.*.backend.write_block = write_block_backend;
    var buf: [16]u8 = @splat(0);
    try std.testing.expectEqual(ok, prim.priv_read_sector(mp, 5, &buf));
    try std.testing.expectEqual(@as(u64, 105), seen_lba);
    try std.testing.expectEqual(@as(u32, 1), seen_count);
    try std.testing.expectEqual(@as(u8, 0xAA), buf[0]);
}

test "sector write forwards to the backend with the partition base added" {
    seen_lba = 0;
    seen_count = 0;
    backend_err = 0;
    fake.reset(c.k_ra8_fs_type_fat16);
    const mp = @constCast(fake.mount());
    mp.*.partition_base_lba = 100;
    mp.*.backend.read_block = read_block_backend;
    mp.*.backend.write_block = write_block_backend;
    const buf: [16]u8 = @splat(0x77);
    try std.testing.expectEqual(ok, prim.priv_write_sector(mp, 9, &buf));
    try std.testing.expectEqual(@as(u64, 109), seen_lba);
    try std.testing.expectEqual(@as(u32, 1), seen_count);
}

test "backend errors propagate through the sector wrappers" {
    backend_err = 0xBEEF;
    fake.reset(c.k_ra8_fs_type_fat16);
    const mp = @constCast(fake.mount());
    mp.*.backend.read_block = read_block_backend;
    mp.*.backend.write_block = write_block_backend;
    var buf: [16]u8 = @splat(0);
    try std.testing.expectEqual(@as(u16, 0xBEEF), prim.priv_read_sector(mp, 1, &buf));
    try std.testing.expectEqual(@as(u16, 0xBEEF), prim.priv_write_sector(mp, 1, &buf));
    backend_err = 0;
}
