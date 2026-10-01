//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the bounds one incoming protobuf field is taken under.

const std = @import("std");
const field_copy = @import("implementation").field_copy;

test "a short field is taken whole" {
    try std.testing.expectEqual(@as(usize, 4), field_copy.strTake(4, 33));
}

test "a field exactly filling the room is taken whole" {
    try std.testing.expectEqual(@as(usize, 32), field_copy.strTake(32, 33));
}

test "a longer field is truncated to the room, not refused" {
    try std.testing.expectEqual(@as(usize, 32), field_copy.strTake(64, 33));
}

test "the terminator's octet is never given to the field" {
    var cap: u8 = 1;
    while (cap < 64) : (cap += 1) {
        try std.testing.expect(field_copy.strTake(1000, cap) < cap);
    }
}

test "a one-octet destination holds only the terminator" {
    try std.testing.expectEqual(@as(usize, 0), field_copy.strTake(9, 1));
}

test "a destination with no capacity takes nothing" {
    try std.testing.expectEqual(@as(usize, 0), field_copy.strTake(9, 0));
}

test "an empty field takes nothing whatever the capacity" {
    try std.testing.expectEqual(@as(usize, 0), field_copy.strTake(0, 33));
    try std.testing.expectEqual(@as(usize, 0), field_copy.strTake(0, 0));
}

test "six octets are one hardware address" {
    try std.testing.expect(field_copy.macAcceptable(6));
}

test "a short field is not an address" {
    try std.testing.expect(!field_copy.macAcceptable(5));
    try std.testing.expect(!field_copy.macAcceptable(0));
}

test "a long field is not an address either" {
    try std.testing.expect(!field_copy.macAcceptable(7));
    try std.testing.expect(!field_copy.macAcceptable(1600));
}

test "the address bound is the one the public header fixes" {
    try std.testing.expectEqual(@as(usize, 6), field_copy.Bound.mac_octets);
}

test "exactly one length is an address" {
    var len: usize = 0;
    var accepted: usize = 0;
    while (len <= 64) : (len += 1) {
        if (field_copy.macAcceptable(len)) accepted += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), accepted);
}
