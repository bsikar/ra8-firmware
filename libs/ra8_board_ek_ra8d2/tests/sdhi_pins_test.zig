//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SDHI0 bus routing.

const std = @import("std");
const sdhi_pins = @import("sdhi_pins");

var routed: [16]u16 = undefined;
var routed_psel: [16]u32 = undefined;
var routed_owner: [*:0]const u8 = "";
var routed_count: usize = 0;
var fail_at: usize = 0xFF;

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    if (routed_count < routed.len) {
        routed[routed_count] = pin;
        routed_psel[routed_count] = psel;
    }
    routed_owner = owner;
    routed_count += 1;
    return if (routed_count - 1 == fail_at) 0x307 else 0;
}

fn reset() void {
    routed_count = 0;
    fail_at = 0xFF;
}

test "eight pins, P400 through P407 in bus order" {
    try std.testing.expectEqual(@as(usize, 8), sdhi_pins.bus.len);
    for (sdhi_pins.bus, 0..) |pin, i| {
        try std.testing.expectEqual(@as(u16, @intCast(0x0400 + i)), pin);
    }
}

test "init routes all eight to the SDHI function" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), sdhi_pins.init());
    try std.testing.expectEqual(@as(usize, 8), routed_count);
    for (0..8) |i| {
        try std.testing.expectEqual(sdhi_pins.bus[i], routed[i]);
        try std.testing.expectEqual(@as(u32, 0x15), routed_psel[i]);
    }
}

test "the route is claimed under the board's own owner tag" {
    reset();
    _ = sdhi_pins.init();
    try std.testing.expectEqualStrings("ra8_board.sdhi", std.mem.span(routed_owner));
}

test "a routing failure stops at the failing pin" {
    reset();
    fail_at = 2;
    try std.testing.expectEqual(@as(u32, 0x307), sdhi_pins.init());
    try std.testing.expectEqual(@as(usize, 3), routed_count);
}
