//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Arduino header GPIO modes.

const std = @import("std");
const arduino = @import("arduino");

var input_calls: u32 = 0;
var output_calls: u32 = 0;
var last_pin: u16 = 0xFFFF;
var last_pull: u32 = 0xFFFF_FFFF;
var last_level: u32 = 0xFFFF_FFFF;
var read_level: u32 = 1;
var err_to_return: u32 = 0;

export fn ra8_gpio_input_init(pin: u16, pull: u32) u32 {
    input_calls += 1;
    last_pin = pin;
    last_pull = pull;
    return err_to_return;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    output_calls += 1;
    last_pin = pin;
    last_level = init_level;
    return err_to_return;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    last_pin = pin;
    last_level = level;
    return err_to_return;
}

export fn ra8_gpio_read(pin: u16, out_level: *u32) u32 {
    last_pin = pin;
    out_level.* = read_level;
    return err_to_return;
}

fn reset() void {
    input_calls = 0;
    output_calls = 0;
    last_pin = 0xFFFF;
    last_pull = 0xFFFF_FFFF;
    last_level = 0xFFFF_FFFF;
    err_to_return = 0;
}

test "input mode takes no pull" {
    reset();
    // A3 is P004.
    try std.testing.expectEqual(@as(u32, 0), arduino.pinInit(0x0004, 0));
    try std.testing.expectEqual(@as(u32, 1), input_calls);
    try std.testing.expectEqual(@as(u16, 0x0004), last_pin);
    try std.testing.expectEqual(@as(u32, 0), last_pull);
}

test "input pullup mode asks for the pull" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), arduino.pinInit(0x0014, 1));
    try std.testing.expectEqual(@as(u32, 1), input_calls);
    try std.testing.expectEqual(@as(u32, 1), last_pull);
}

test "output mode starts low" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), arduino.pinInit(0x0015, 2));
    try std.testing.expectEqual(@as(u32, 1), output_calls);
    try std.testing.expectEqual(@as(u32, 0), last_level);
}

test "an unknown mode is refused without touching the pin" {
    reset();
    try std.testing.expect(arduino.pinInit(0x0004, 3) != 0);
    try std.testing.expect(arduino.pinInit(0x0004, 255) != 0);
    try std.testing.expectEqual(@as(u32, 0), input_calls);
    try std.testing.expectEqual(@as(u32, 0), output_calls);
}

test "write and read pass straight through" {
    reset();
    _ = arduino.write(0x0004, 1);
    try std.testing.expectEqual(@as(u16, 0x0004), last_pin);
    try std.testing.expectEqual(@as(u32, 1), last_level);

    read_level = 0;
    var level: u32 = 0xFF;
    try std.testing.expectEqual(@as(u32, 0), arduino.read(0x0014, &level));
    try std.testing.expectEqual(@as(u32, 0), level);
}

test "an error from the GPIO layer is returned unchanged" {
    reset();
    err_to_return = 0x308;
    try std.testing.expectEqual(@as(u32, 0x308), arduino.pinInit(0x0004, 2));
    try std.testing.expectEqual(@as(u32, 0x308), arduino.write(0x0004, 1));
    var level: u32 = 0;
    try std.testing.expectEqual(@as(u32, 0x308), arduino.read(0x0004, &level));
}
