//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The startup zero-fill contract.
//!
//! The half-open span is the part worth pinning here, because getting it
//! wrong is silent: an inclusive fill clobbers the byte after the region and
//! nothing reports it. The C suite
//! (`tests/core/src/test_ra8_boot_region.c`) asserts the same property from
//! the other side of the ABI, through guard bytes on both ends.

const std = @import("std");

const region = @import("boot_region");

test "a span is half-open, so the byte at end is not counted" {
    var buf: [16]u8 = @splat(0xA5);
    const first = @intFromPtr(&buf[4]);
    const last = @intFromPtr(&buf[12]);
    try std.testing.expectEqual(@as(?usize, 8), region.spanLength(first, last));
}

test "an empty span is a length of zero, not a refusal" {
    var buf: [16]u8 = @splat(0xA5);
    const at = @intFromPtr(&buf[4]);
    try std.testing.expectEqual(@as(?usize, 0), region.spanLength(at, at));
}

test "end before start has no length at all" {
    var buf: [16]u8 = @splat(0xA5);
    const first = @intFromPtr(&buf[12]);
    const last = @intFromPtr(&buf[4]);
    try std.testing.expectEqual(@as(?usize, null), region.spanLength(first, last));
}

test "zeroing an interior span leaves both neighbours alone" {
    var buf: [16]u8 = @splat(0xA5);
    region.zero(buf[4..12]);
    for (buf[0..4]) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
    for (buf[4..12]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    for (buf[12..]) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
}

test "zeroing an empty slice writes nothing" {
    var buf: [4]u8 = @splat(0xA5);
    region.zero(buf[2..2]);
    for (buf) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
}

test "off target the section is the stand-in window, and the fill clears it" {
    const window = region.sdramSection();
    try std.testing.expectEqual(region.stand_in.bytes, window.len);
    @memset(window, 0xA5);
    region.zero(region.sdramSection());
    for (region.sdramSection()) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}
