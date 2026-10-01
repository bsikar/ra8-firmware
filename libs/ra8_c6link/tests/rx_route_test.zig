//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where one received frame's payload is routed.

const std = @import("std");
const implementation = @import("implementation");

const rx_route = implementation.rx_route;

test "the serial interface carries the control plane" {
    try std.testing.expectEqual(rx_route.Route.rpc, rx_route.routeFor(rx_route.If.serial));
}

test "the station interface carries ethernet data" {
    try std.testing.expectEqual(rx_route.Route.ethernet, rx_route.routeFor(rx_route.If.sta));
}

test "the access-point interface carries ethernet data" {
    try std.testing.expectEqual(rx_route.Route.ethernet, rx_route.routeFor(rx_route.If.ap));
}

test "the privileged interface is counted, not decoded" {
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.privileged));
}

test "the invalid interface is counted" {
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.invalid));
}

test "the bluetooth and diagnostic interfaces are counted" {
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.hci));
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.diagnostic));
}

test "the wired ethernet interface is counted, this build never offers it" {
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.ethernet));
}

test "the end marker is counted" {
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(rx_route.If.max));
}

test "every byte past the declared interfaces is counted" {
    var if_type: u8 = rx_route.If.max;
    while (if_type < 255) : (if_type += 1) {
        try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(if_type));
    }
    try std.testing.expectEqual(rx_route.Route.counted, rx_route.routeFor(255));
}

test "exactly three interfaces route anywhere but the counter" {
    var routed: usize = 0;
    var if_type: u8 = 0;
    while (true) : (if_type += 1) {
        if (rx_route.routeFor(if_type) != .counted) routed += 1;
        if (if_type == 255) break;
    }
    try std.testing.expectEqual(@as(usize, 3), routed);
}

test "the route ordinals are the ABI the C side reads back" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(rx_route.Route.rpc));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(rx_route.Route.ethernet));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(rx_route.Route.counted));
}

test "the interface numbers mirror the vendored header order" {
    try std.testing.expectEqual(@as(u8, 0), rx_route.If.invalid);
    try std.testing.expectEqual(@as(u8, 1), rx_route.If.sta);
    try std.testing.expectEqual(@as(u8, 2), rx_route.If.ap);
    try std.testing.expectEqual(@as(u8, 3), rx_route.If.serial);
    try std.testing.expectEqual(@as(u8, 4), rx_route.If.hci);
    try std.testing.expectEqual(@as(u8, 5), rx_route.If.privileged);
    try std.testing.expectEqual(@as(u8, 6), rx_route.If.diagnostic);
    try std.testing.expectEqual(@as(u8, 7), rx_route.If.ethernet);
    try std.testing.expectEqual(@as(u8, 8), rx_route.If.max);
}
