// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const mss = @import("mss");

test "overhead is one IPv4 and one TCP header" {
    try std.testing.expectEqual(@as(u16, 40), mss.overhead);
}

test "the pinned MTU floor clamps to exactly the MSS floor" {
    try std.testing.expectEqual(@as(?u16, 88), mss.clamp(128));
}

test "an MTU at or below the header overhead has no segment" {
    try std.testing.expectEqual(@as(?u16, null), mss.clamp(40));
    try std.testing.expectEqual(@as(?u16, null), mss.clamp(39));
    try std.testing.expectEqual(@as(?u16, null), mss.clamp(0));
}

test "an MTU leaving less than the MSS floor is rejected at the boundary" {
    try std.testing.expectEqual(@as(?u16, null), mss.clamp(103));
    try std.testing.expectEqual(@as(?u16, 64), mss.clamp(104));
}

test "a typical ethernet MTU clamps to the headroom" {
    try std.testing.expectEqual(@as(?u16, 1460), mss.clamp(1500));
}
