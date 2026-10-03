//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const l3 = @import("layer3_switch");

const good: l3.Cfg = .{ .port_count = 3, .mtu_bytes = 1500, .promiscuous = 1 };

test "open refuses zero ports, then a zero MTU, and leaves the switch closed" {
    var state: l3.State = .{};
    try std.testing.expectError(error.InvalidArg, state.open(.{ .port_count = 0, .mtu_bytes = 0, .promiscuous = 0 }));
    try std.testing.expectError(error.InvalidArg, state.open(.{ .port_count = 1, .mtu_bytes = 0, .promiscuous = 0 }));
    try std.testing.expect(!state.opened);
}

test "open records promiscuous mode and refuses a second open" {
    var state: l3.State = .{};
    try state.open(good);
    try std.testing.expect(state.opened and state.promiscuous);
    try std.testing.expectError(error.Exists, state.open(good));
}

test "close resets both flags and refuses a second close" {
    var state: l3.State = .{};
    try state.open(good);
    try state.close();
    try std.testing.expect(!state.opened and !state.promiscuous);
    try std.testing.expectError(error.InvalidState, state.close());
}

test "routes are not_initialized while closed and not_supported once open" {
    var state: l3.State = .{};
    try std.testing.expectError(error.NotInitialized, state.route());
    try state.open(.{ .port_count = 1, .mtu_bytes = 64, .promiscuous = 0 });
    try std.testing.expect(!state.promiscuous);
    try std.testing.expectError(error.NotSupported, state.route());
}

test "cfg and route match the C header layouts" {
    try std.testing.expectEqual(@as(usize, 6), @sizeOf(l3.Cfg));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(l3.Cfg, "mtu_bytes"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(l3.Cfg, "promiscuous"));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(l3.Route));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(l3.Route, "egress_port"));
}
