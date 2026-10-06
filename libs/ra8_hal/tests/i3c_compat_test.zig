//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_compat.zig.

const std = @import("std");
const compat = @import("i3c_compat");

test "channel range is checked before the mode" {
    const chans = [_]compat.Chan{.{ .initialized = false, .mode = 0 }};
    try std.testing.expectEqual(@as(?u16, 0x103), compat.guard(&chans, 1));
    try std.testing.expectEqual(@as(?u16, 0x103), compat.guard(&chans, 0xFF));
}

test "native mode is refused with invalid_state" {
    const chans = [_]compat.Chan{.{ .initialized = true, .mode = 0 }};
    try std.testing.expectEqual(@as(?u16, 0x104), compat.guard(&chans, 0));
}

test "I2C mode forwards even before init, as in the C" {
    const chans = [_]compat.Chan{.{ .initialized = false, .mode = compat.mode_i2c }};
    try std.testing.expectEqual(@as(?u16, null), compat.guard(&chans, 0));
}

test "Chan matches the C struct" {
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(compat.Chan));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(compat.Chan, "mode"));
}
