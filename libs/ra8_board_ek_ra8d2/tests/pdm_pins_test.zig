//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The microphone pin map and the order the two routes go out in.

const std = @import("std");
const pdm_pins = @import("pdm_pins");

const Err = struct {
    const ok: u32 = 0;
    const gpio_conflict: u32 = 0x501;
};

var routed_pins: [8]u16 = undefined;
var routed_psel: [8]u32 = undefined;
var routed_owner: [8][*:0]const u8 = undefined;
var routed: usize = 0;
var fail_at: usize = 0xFF;

fn reset() void {
    routed = 0;
    fail_at = 0xFF;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    if (routed == fail_at) return Err.gpio_conflict;
    routed_pins[routed] = pin;
    routed_psel[routed] = psel;
    routed_owner[routed] = owner;
    routed += 1;
    return Err.ok;
}

test "the clock pin is P8_12" {
    try std.testing.expectEqual(@as(u16, (8 << 8) | 12), pdm_pins.clk);
}

test "the data pin is P5_2" {
    try std.testing.expectEqual(@as(u16, (5 << 8) | 2), pdm_pins.dat);
}

test "both microphone pins are in the route table" {
    try std.testing.expectEqual(@as(usize, 2), pdm_pins.routes.len);
}

test "routing both pins reports ok" {
    reset();
    try std.testing.expectEqual(Err.ok, pdm_pins.routeAll());
}

test "both pins are routed" {
    reset();
    _ = pdm_pins.routeAll();
    try std.testing.expectEqual(@as(usize, 2), routed);
}

test "the clock pin is routed first" {
    reset();
    _ = pdm_pins.routeAll();
    try std.testing.expectEqual(pdm_pins.clk, routed_pins[0]);
    try std.testing.expectEqual(pdm_pins.dat, routed_pins[1]);
}

test "both pins go to the PDM-IF function" {
    reset();
    _ = pdm_pins.routeAll();
    try std.testing.expectEqual(@as(u32, 0x1B), routed_psel[0]);
    try std.testing.expectEqual(@as(u32, 0x1B), routed_psel[1]);
}

test "each pin carries its own owner string" {
    reset();
    _ = pdm_pins.routeAll();
    try std.testing.expectEqualStrings("board.pdm.clk", std.mem.span(routed_owner[0]));
    try std.testing.expectEqualStrings("board.pdm.dat", std.mem.span(routed_owner[1]));
}

test "a conflict on the clock pin is returned" {
    reset();
    fail_at = 0;
    try std.testing.expectEqual(Err.gpio_conflict, pdm_pins.routeAll());
}

test "a conflict on the clock pin stops before the data pin" {
    reset();
    fail_at = 0;
    _ = pdm_pins.routeAll();
    try std.testing.expectEqual(@as(usize, 0), routed);
}

test "a conflict on the data pin is returned" {
    reset();
    fail_at = 1;
    try std.testing.expectEqual(Err.gpio_conflict, pdm_pins.routeAll());
}

test "a conflict on the data pin still leaves the clock pin routed" {
    reset();
    fail_at = 1;
    _ = pdm_pins.routeAll();
    try std.testing.expectEqual(@as(usize, 1), routed);
    try std.testing.expectEqual(pdm_pins.clk, routed_pins[0]);
}
