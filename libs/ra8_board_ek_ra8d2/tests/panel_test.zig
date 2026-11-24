//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The panel reset strap and backlight enable.

const std = @import("std");
const panel = @import("panel");

const Step = struct { pin: u16, level: u32, kind: u8 };

const kind_output_init: u8 = 0;
const kind_write: u8 = 1;
const kind_delay: u8 = 2;

var steps: [8]Step = undefined;
var count: usize = 0;
var fail_at: usize = 0xFF;
var delays: [4]u32 = .{ 0, 0, 0, 0 };
var delay_count: usize = 0;

fn push(pin: u16, level: u32, kind: u8) u32 {
    if (count < steps.len) steps[count] = .{ .pin = pin, .level = level, .kind = kind };
    count += 1;
    return if (count - 1 == fail_at) 0x304 else 0;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    return push(pin, init_level, kind_output_init);
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    return push(pin, level, kind_write);
}

export fn ra8_delay_ms(milliseconds: u32) void {
    if (delay_count < delays.len) delays[delay_count] = milliseconds;
    delay_count += 1;
}

fn reset() void {
    count = 0;
    delay_count = 0;
    fail_at = 0xFF;
}

test "power on pulses reset low then high, then enables the backlight" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), panel.powerOn());
    try std.testing.expectEqual(@as(usize, 3), count);

    // RESET_L is P606, driven low as an output first.
    try std.testing.expectEqual(@as(u16, 0x0606), steps[0].pin);
    try std.testing.expectEqual(kind_output_init, steps[0].kind);
    try std.testing.expectEqual(@as(u32, 0), steps[0].level);

    try std.testing.expectEqual(@as(u16, 0x0606), steps[1].pin);
    try std.testing.expectEqual(kind_write, steps[1].kind);
    try std.testing.expectEqual(@as(u32, 1), steps[1].level);

    // BLEN is P514, brought up high.
    try std.testing.expectEqual(@as(u16, 0x050E), steps[2].pin);
    try std.testing.expectEqual(kind_output_init, steps[2].kind);
    try std.testing.expectEqual(@as(u32, 1), steps[2].level);
}

test "both halves of the reset pulse wait 50 ms" {
    reset();
    _ = panel.powerOn();
    try std.testing.expectEqual(@as(usize, 2), delay_count);
    try std.testing.expectEqual(@as(u32, 50), delays[0]);
    try std.testing.expectEqual(@as(u32, 50), delays[1]);
}

test "a failure on the reset assert stops before the release" {
    reset();
    fail_at = 0;
    try std.testing.expectEqual(@as(u32, 0x304), panel.powerOn());
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(@as(usize, 0), delay_count);
}

test "a failure on the reset release stops before the backlight" {
    reset();
    fail_at = 1;
    try std.testing.expectEqual(@as(u32, 0x304), panel.powerOn());
    try std.testing.expectEqual(@as(usize, 2), count);
    try std.testing.expectEqual(@as(usize, 1), delay_count);
}

test "backlight drives BLEN high and low without re-initialising it" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), panel.backlight(true));
    try std.testing.expectEqual(@as(u16, 0x050E), steps[0].pin);
    try std.testing.expectEqual(kind_write, steps[0].kind);
    try std.testing.expectEqual(@as(u32, 1), steps[0].level);

    reset();
    _ = panel.backlight(false);
    try std.testing.expectEqual(@as(u32, 0), steps[0].level);
    try std.testing.expectEqual(kind_write, steps[0].kind);
}

test "backlightInit configures BLEN as output without touching RESET_L delays" {
    reset();
    try std.testing.expectEqual(@as(u16, 0), panel.backlightInit(true));
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(@as(usize, 0), delay_count);
    try std.testing.expectEqual(@as(u16, 0x050E), steps[0].pin);
    try std.testing.expect(steps[0].kind != kind_write);
    try std.testing.expectEqual(@as(u32, 1), steps[0].level);

    reset();
    try std.testing.expectEqual(@as(u16, 0), panel.backlightInit(false));
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(@as(u32, 0), steps[0].level);
}

test "backlightCaps validates null and reports binary EK-RA8D2 hardware" {
    try std.testing.expectEqual(@as(u16, 0x504), panel.backlightCaps(null));
    var caps: panel.BacklightCaps = undefined;
    try std.testing.expectEqual(@as(u16, 0), panel.backlightCaps(&caps));
    try std.testing.expectEqual(@as(u8, 0), caps.min_pct);
    try std.testing.expectEqual(@as(u8, 100), caps.max_pct);
    try std.testing.expectEqual(false, caps.supports_pwm);
}

test "backlightBrightness maps 0/100 to BLEN and fails closed on 1..99 and >100" {
    reset();
    try std.testing.expectEqual(@as(u16, 0), panel.backlightBrightness(100));
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(kind_write, steps[0].kind);
    try std.testing.expectEqual(@as(u32, 1), steps[0].level);

    reset();
    try std.testing.expectEqual(@as(u16, 0), panel.backlightBrightness(0));
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expectEqual(@as(u32, 0), steps[0].level);

    reset();
    try std.testing.expectEqual(@as(u16, 0x107), panel.backlightBrightness(1));
    try std.testing.expectEqual(@as(u16, 0x107), panel.backlightBrightness(50));
    try std.testing.expectEqual(@as(u16, 0x107), panel.backlightBrightness(99));
    try std.testing.expectEqual(@as(usize, 0), count);

    try std.testing.expectEqual(@as(u16, 0x103), panel.backlightBrightness(101));
    try std.testing.expectEqual(@as(u16, 0x103), panel.backlightBrightness(255));
    try std.testing.expectEqual(@as(usize, 0), count);
}
