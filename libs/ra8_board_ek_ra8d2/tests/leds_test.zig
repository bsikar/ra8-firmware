//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! LED id to pin mapping and the four drive paths.

const std = @import("std");
const leds = @import("leds");

var last_pin: u16 = 0xFFFF;
var last_level: u32 = 0xFFFF_FFFF;
var toggles: u32 = 0;
var init_calls: u32 = 0;
var err_to_return: u32 = 0;

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    init_calls += 1;
    last_pin = pin;
    last_level = init_level;
    return err_to_return;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    last_pin = pin;
    last_level = level;
    return err_to_return;
}

export fn ra8_gpio_toggle(pin: u16) u32 {
    toggles += 1;
    last_pin = pin;
    return err_to_return;
}

fn reset() void {
    last_pin = 0xFFFF;
    last_level = 0xFFFF_FFFF;
    toggles = 0;
    init_calls = 0;
    err_to_return = 0;
}

test "three LEDs, on the pins the UM lists" {
    try std.testing.expectEqual(@as(usize, 3), leds.pins.len);
    // LED1 blue P600, LED2 green P303, LED3 red PA07 (port 10, pin 7).
    try std.testing.expectEqual(@as(u16, 0x0600), leds.pins[0]);
    try std.testing.expectEqual(@as(u16, 0x0303), leds.pins[1]);
    try std.testing.expectEqual(@as(u16, 0x0A07), leds.pins[2]);
}

test "pinOf refuses an id past the end" {
    try std.testing.expect(leds.pinOf(0) != null);
    try std.testing.expect(leds.pinOf(2) != null);
    try std.testing.expect(leds.pinOf(3) == null);
    try std.testing.expect(leds.pinOf(255) == null);
}

test "readPin writes the pin and leaves it alone on a bad id" {
    var pin: u16 = 0xDEAD;
    try std.testing.expectEqual(@as(u32, 0), leds.readPin(1, &pin));
    try std.testing.expectEqual(@as(u16, 0x0303), pin);

    pin = 0xDEAD;
    try std.testing.expect(leds.readPin(9, &pin) != 0);
    try std.testing.expectEqual(@as(u16, 0xDEAD), pin);
}

test "init drives the pin low so a fresh LED is dark" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), leds.init(2));
    try std.testing.expectEqual(@as(u32, 1), init_calls);
    try std.testing.expectEqual(@as(u16, 0x0A07), last_pin);
    try std.testing.expectEqual(@as(u32, 0), last_level);
}

test "on and off drive high and low" {
    reset();
    _ = leds.on(0);
    try std.testing.expectEqual(@as(u16, 0x0600), last_pin);
    try std.testing.expectEqual(@as(u32, 1), last_level);

    _ = leds.off(0);
    try std.testing.expectEqual(@as(u32, 0), last_level);
}

test "toggle goes to the toggle primitive, not a read-modify-write" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), leds.toggle(1));
    try std.testing.expectEqual(@as(u32, 1), toggles);
    try std.testing.expectEqual(@as(u16, 0x0303), last_pin);
}

test "a bad id never reaches the GPIO layer" {
    reset();
    try std.testing.expect(leds.init(3) != 0);
    try std.testing.expect(leds.on(3) != 0);
    try std.testing.expect(leds.off(3) != 0);
    try std.testing.expect(leds.toggle(3) != 0);
    try std.testing.expectEqual(@as(u32, 0), init_calls);
    try std.testing.expectEqual(@as(u32, 0), toggles);
    try std.testing.expectEqual(@as(u16, 0xFFFF), last_pin);
}

test "a GPIO failure is passed back, not swallowed" {
    reset();
    err_to_return = 0x301;
    try std.testing.expectEqual(@as(u32, 0x301), leds.on(0));
    try std.testing.expectEqual(@as(u32, 0x301), leds.init(0));
    try std.testing.expectEqual(@as(u32, 0x301), leds.toggle(0));
}
