//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_copy_str` and `priv_c6link_copy_mac` through their C ABI.

const std = @import("std");
const field_abi = @import("field_abi");

const c = field_abi.c;
const BinaryData = field_abi.BinaryData;

fn field(bytes: []const u8) BinaryData {
    return .{ .len = bytes.len, .data = bytes.ptr };
}

test "a text field that fits is copied whole and terminated" {
    var dst: [8]u8 = @splat(0xFF);
    const src = field("abc");
    try std.testing.expectEqual(@as(u8, 3), field_abi.priv_c6link_copy_str(&dst, dst.len, &src));
    try std.testing.expectEqualSlices(u8, "abc\x00", dst[0..4]);
}

test "a long text field is truncated to leave room for the terminator" {
    var dst: [4]u8 = @splat(0xFF);
    const src = field("abcdefgh");
    try std.testing.expectEqual(@as(u8, 3), field_abi.priv_c6link_copy_str(&dst, dst.len, &src));
    try std.testing.expectEqualSlices(u8, "abc\x00", &dst);
}

test "a missing field still terminates; no room or no destination copies nothing" {
    var dst: [4]u8 = @splat(0xFF);
    try std.testing.expectEqual(@as(u8, 0), field_abi.priv_c6link_copy_str(&dst, dst.len, null));
    try std.testing.expectEqual(@as(u8, 0), dst[0]);

    dst[0] = 0xFF;
    const empty: BinaryData = .{ .len = 5, .data = null };
    try std.testing.expectEqual(@as(u8, 0), field_abi.priv_c6link_copy_str(&dst, dst.len, &empty));
    try std.testing.expectEqual(@as(u8, 0), dst[0]);

    dst[0] = 0xFF;
    const src = field("abc");
    try std.testing.expectEqual(@as(u8, 0), field_abi.priv_c6link_copy_str(&dst, 0, &src));
    try std.testing.expectEqual(@as(u8, 0xFF), dst[0]);
    try std.testing.expectEqual(@as(u8, 0), field_abi.priv_c6link_copy_str(null, 4, &src));
}

test "an exact six-octet field is copied as an address" {
    var mac = std.mem.zeroes(c.ra8_c6link_mac_t);
    const src = field(&.{ 1, 2, 3, 4, 5, 6 });
    try std.testing.expect(field_abi.priv_c6link_copy_mac(&mac, &src));
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6 }, &mac.octet);
}

test "a short, long or missing address field is refused and zeroes the destination" {
    var mac: c.ra8_c6link_mac_t = .{ .octet = .{ 9, 9, 9, 9, 9, 9 } };
    const short = field(&.{ 1, 2, 3 });
    try std.testing.expect(!field_abi.priv_c6link_copy_mac(&mac, &short));
    try std.testing.expectEqualSlices(u8, &.{ 0, 0, 0, 0, 0, 0 }, &mac.octet);

    mac.octet[0] = 9;
    const long = field(&.{ 1, 2, 3, 4, 5, 6, 7 });
    try std.testing.expect(!field_abi.priv_c6link_copy_mac(&mac, &long));
    try std.testing.expectEqual(@as(u8, 0), mac.octet[0]);

    mac.octet[0] = 9;
    try std.testing.expect(!field_abi.priv_c6link_copy_mac(&mac, null));
    try std.testing.expectEqual(@as(u8, 0), mac.octet[0]);
    try std.testing.expect(!field_abi.priv_c6link_copy_mac(null, &short));
}
