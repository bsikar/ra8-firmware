//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Octo-SPI flash reset strap and bus routing.

const std = @import("std");
const xspi_pins = @import("xspi_pins");

var routed: [16]u16 = undefined;
var routed_psel: [16]u32 = undefined;
var routed_count: usize = 0;
var gpio_steps: usize = 0;
var last_gpio_pin: u16 = 0xFFFF;
var last_gpio_level: u32 = 0xFFFF_FFFF;
var delays: [4]u32 = .{ 0, 0, 0, 0 };
var delay_count: usize = 0;
var route_fail_at: usize = 0xFF;
var gpio_err: u32 = 0;

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = owner;
    if (routed_count < routed.len) {
        routed[routed_count] = pin;
        routed_psel[routed_count] = psel;
    }
    routed_count += 1;
    return if (routed_count - 1 == route_fail_at) 0x305 else 0;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    gpio_steps += 1;
    last_gpio_pin = pin;
    last_gpio_level = init_level;
    return gpio_err;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    gpio_steps += 1;
    last_gpio_pin = pin;
    last_gpio_level = level;
    return gpio_err;
}

export fn ra8_delay_ms(milliseconds: u32) void {
    if (delay_count < delays.len) delays[delay_count] = milliseconds;
    delay_count += 1;
}

fn reset() void {
    routed_count = 0;
    gpio_steps = 0;
    delay_count = 0;
    route_fail_at = 0xFF;
    gpio_err = 0;
}

test "eleven bus pins, and RESET_L is not one of them" {
    try std.testing.expectEqual(@as(usize, 11), xspi_pins.bus.len);
    for (xspi_pins.bus) |pin| try std.testing.expect(pin != xspi_pins.reset_pin);
    // RESET_L is P106, a plain GPIO strap.
    try std.testing.expectEqual(@as(u16, 0x0106), xspi_pins.reset_pin);
}

test "the bus pins are the OCTA set from the UM table" {
    // CS P104, CK P808, DQS P801, then DQ0..DQ7.
    try std.testing.expectEqual(@as(u16, 0x0104), xspi_pins.bus[0]);
    try std.testing.expectEqual(@as(u16, 0x0808), xspi_pins.bus[1]);
    try std.testing.expectEqual(@as(u16, 0x0801), xspi_pins.bus[2]);
    try std.testing.expectEqual(@as(u16, 0x0100), xspi_pins.bus[3]);
    try std.testing.expectEqual(@as(u16, 0x0804), xspi_pins.bus[10]);
}

test "no bus pin is listed twice" {
    for (xspi_pins.bus, 0..) |a, i| {
        for (xspi_pins.bus[i + 1 ..]) |b| try std.testing.expect(a != b);
    }
}

test "init straps reset before routing anything" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), xspi_pins.init());
    try std.testing.expectEqual(@as(usize, 2), gpio_steps);
    try std.testing.expectEqual(@as(usize, 11), routed_count);
    // Both GPIO steps are the strap, so the last one is still RESET_L.
    try std.testing.expectEqual(xspi_pins.reset_pin, last_gpio_pin);
    try std.testing.expectEqual(@as(u32, 1), last_gpio_level);
}

test "every bus pin is routed to the xSPI function" {
    reset();
    _ = xspi_pins.init();
    try std.testing.expectEqual(@as(usize, 11), routed_count);
    for (0..routed_count) |i| {
        try std.testing.expectEqual(@as(u32, 0x1C), routed_psel[i]);
        try std.testing.expectEqual(xspi_pins.bus[i], routed[i]);
    }
}

test "the release wait is the power-up window, not the reset recovery" {
    reset();
    _ = xspi_pins.init();
    try std.testing.expectEqual(@as(usize, 2), delay_count);
    try std.testing.expectEqual(@as(u32, 1), delays[0]);
    try std.testing.expectEqual(@as(u32, 15), delays[1]);
}

test "a GPIO failure on the strap stops before the bus is routed" {
    reset();
    gpio_err = 0x306;
    try std.testing.expectEqual(@as(u32, 0x306), xspi_pins.init());
    try std.testing.expectEqual(@as(usize, 0), routed_count);
}

test "a routing failure stops the loop at the failing pin" {
    reset();
    route_fail_at = 3;
    try std.testing.expectEqual(@as(u32, 0x305), xspi_pins.init());
    try std.testing.expectEqual(@as(usize, 4), routed_count);
}
