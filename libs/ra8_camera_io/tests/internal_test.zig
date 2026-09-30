//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the pure core: the C record layouts and the write outcome.

const std = @import("std");
const core = @import("implementation");

test "the C record layouts are what the header describes" {
    const ptr_bytes = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(core.Buffer, "data"));
    try std.testing.expectEqual(ptr_bytes, @offsetOf(core.Buffer, "capacity"));
    try std.testing.expectEqual(ptr_bytes, @offsetOf(core.Frame, "bytes"));
    try std.testing.expectEqual(ptr_bytes + 8, @offsetOf(core.Frame, "width"));
    try std.testing.expectEqual(ptr_bytes + 12, @offsetOf(core.Frame, "format"));
}

test "a default frame is the zero-initialised C aggregate" {
    const frame: core.Frame = .{};
    try std.testing.expect(frame.data == null);
    try std.testing.expectEqual(@as(u32, 0), frame.bytes);
    try std.testing.expectEqual(@as(u32, 0), frame.stride_bytes);
    try std.testing.expectEqual(core.Format.rgb888, frame.format);
}

test "the format enum keeps the header's numbering" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(core.Format.rgb888));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(core.Format.uyvy422));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(core.Format.jpeg));
}

test "an unknown format value round-trips rather than trapping" {
    const frame: core.Frame = .{ .format = @enumFromInt(9) };
    try std.testing.expectEqual(@as(u8, 9), @intFromEnum(frame.format));
}

test "only a successful sink can produce the bridge's own invalid_size" {
    try std.testing.expectEqual(core.err.invalid_size, core.writeOutcome(core.err.ok, 1, 2));
    try std.testing.expectEqual(@as(u16, 0x404), core.writeOutcome(0x404, 1, 2));
}
