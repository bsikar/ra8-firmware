//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/rsip_protected.zig (RA8FW-789).

const std = @import("std");
const p = @import("rsip_protected");

test "keyBytes divides by 8 and caps at 32 bytes" {
    try std.testing.expectEqual(@as(?usize, 16), p.keyBytes(128));
    try std.testing.expectEqual(@as(?usize, 24), p.keyBytes(192));
    try std.testing.expectEqual(@as(?usize, 32), p.keyBytes(256));
    try std.testing.expectEqual(@as(?usize, 8), p.keyBytes(64));
    try std.testing.expectEqual(@as(?usize, null), p.keyBytes(264));
}

test "modBytes covers the four RSA sizes" {
    try std.testing.expectEqual(@as(?u32, 128), p.modBytes(1024));
    try std.testing.expectEqual(@as(?u32, 512), p.modBytes(4096));
    try std.testing.expectEqual(@as(?u32, null), p.modBytes(512));
}

test "installCmd maps 2048/3072/4096 and leaves 1024 invalid" {
    try std.testing.expectEqual(@as(u32, 13), p.installCmd(2048));
    try std.testing.expectEqual(@as(u32, 15), p.installCmd(3072));
    try std.testing.expectEqual(@as(u32, 17), p.installCmd(4096));
    try std.testing.expectEqual(p.oem_invalid, p.installCmd(1024));
}

test "eccParams for the four supported curves" {
    try std.testing.expectEqual(@as(u32, 23), p.eccParams(2).?.alg);
    try std.testing.expectEqual(@as(u32, 48), p.eccParams(3).?.priv_bytes);
    try std.testing.expectEqual(@as(u32, 66), p.eccParams(4).?.priv_bytes);
    try std.testing.expectEqual(@as(u32, 35), p.eccParams(9).?.alg);
    try std.testing.expect(p.eccParams(8) == null);
    try std.testing.expect(p.eccParams(0) == null);
}

test "scrub zeroes only the given slice" {
    var buf = [_]u8{0xAA} ** 8;
    p.scrub(buf[2..6]);
    try std.testing.expectEqualSlices(u8, &.{ 0xAA, 0xAA, 0, 0, 0, 0, 0xAA, 0xAA }, &buf);
}

test "payload offset leaves room for the largest key" {
    try std.testing.expect(p.off_payload + p.aes_max_bytes <= p.wrapped_max_payload);
    try std.testing.expectEqual(@as(usize, 20), p.off_payload);
}
