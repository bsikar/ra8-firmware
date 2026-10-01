//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XCLK divider arithmetic and the order the GPT is brought up in.

const std = @import("std");
const camera_xclk = @import("camera_xclk");

const Err = struct {
    const ok: u32 = 0;
    const invalid_arg: u32 = 0x103;
    const not_found: u32 = 0x106;
};

const Step = enum { clock, init, pin, route, drive };

var trace: [16]Step = undefined;
var steps: usize = 0;

var pclkd_hz: u32 = 240_000_000;
var clock_err: u32 = Err.ok;
var init_err: u32 = Err.ok;
var pin_err: u32 = Err.ok;
var route_err: u32 = Err.ok;
var drive_err: u32 = Err.ok;

var seen_channel: u8 = 0xFF;
var seen_period: u32 = 0;
var seen_duty_a: u32 = 0;
var seen_duty_b: u32 = 0xFFFF_FFFF;
var seen_mode: u8 = 0xFF;
var seen_prescaler: u8 = 0xFF;
var seen_auto_start: bool = false;
var seen_pin: u8 = 0xFF;
var seen_output_enable: bool = false;
var seen_route_pin: u16 = 0;
var seen_psel: u32 = 0xFFFF;
var seen_owner: [*:0]const u8 = "";
var seen_drive_pin: u16 = 0;
var seen_dscr: u8 = 0xFF;

fn note(s: Step) void {
    trace[steps] = s;
    steps += 1;
}

fn reset() void {
    steps = 0;
    pclkd_hz = 240_000_000;
    clock_err = Err.ok;
    init_err = Err.ok;
    pin_err = Err.ok;
    route_err = Err.ok;
    drive_err = Err.ok;
}

export fn ra8_cgc_get_clock_hz(id: u32, out_hz: *u32) u32 {
    _ = id;
    note(.clock);
    if (clock_err != Err.ok) return clock_err;
    out_hz.* = pclkd_hz;
    return Err.ok;
}

export fn ra8_gpt_init(channel: u8, cfg: *const anyopaque) u32 {
    const c: *const extern struct {
        mode: u8,
        prescaler: u8,
        period: u32,
        duty_a: u32,
        duty_b: u32,
        auto_start: bool,
    } = @ptrCast(@alignCast(cfg));
    note(.init);
    seen_channel = channel;
    seen_mode = c.mode;
    seen_prescaler = c.prescaler;
    seen_period = c.period;
    seen_duty_a = c.duty_a;
    seen_duty_b = c.duty_b;
    seen_auto_start = c.auto_start;
    return init_err;
}

export fn ra8_gpt_pwm_pin_configure(channel: u8, pin: u8, cfg: *const anyopaque) u32 {
    const c: *const extern struct {
        output_enable: bool,
        polarity: u8,
        stop_level: u8,
        disable_on_fault: u8,
    } = @ptrCast(@alignCast(cfg));
    _ = channel;
    note(.pin);
    seen_pin = pin;
    seen_output_enable = c.output_enable;
    return pin_err;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    note(.route);
    seen_route_pin = pin;
    seen_psel = psel;
    seen_owner = owner;
    return route_err;
}

export fn ra8_pfs_set_drive_strength(pin: u16, dscr: u8) u32 {
    note(.drive);
    seen_drive_pin = pin;
    seen_dscr = dscr;
    return drive_err;
}

test "a zero frequency is refused before the clock is even read" {
    reset();
    try std.testing.expectEqual(Err.invalid_arg, camera_xclk.start(0));
    try std.testing.expectEqual(0, steps);
}

test "a failing clock read is returned as-is" {
    reset();
    clock_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera_xclk.start(24_000_000));
    try std.testing.expectEqual(1, steps);
}

test "a frequency too high for a whole divider is refused" {
    reset();
    // 240 MHz / 160 MHz = 1, below the two-count minimum for a square wave.
    try std.testing.expectEqual(Err.invalid_arg, camera_xclk.start(160_000_000));
    try std.testing.expectEqual(1, steps);
}

test "a frequency needing a divider past GTPR's 16 bits is refused" {
    reset();
    // 240 MHz / 3000 Hz = 80000, past 0xFFFF.
    try std.testing.expectEqual(Err.invalid_arg, camera_xclk.start(3_000));
    try std.testing.expectEqual(1, steps);
}

test "the exact edges of the divider range are accepted" {
    reset();
    pclkd_hz = 2;
    try std.testing.expectEqual(Err.ok, camera_xclk.start(1));
    try std.testing.expectEqual(1, seen_period);

    reset();
    pclkd_hz = 0xFFFF;
    try std.testing.expectEqual(Err.ok, camera_xclk.start(1));
    try std.testing.expectEqual(0xFFFE, seen_period);
}

test "24 MHz off 240 MHz PCLKD programs a 10-count period at half duty" {
    reset();
    try std.testing.expectEqual(Err.ok, camera_xclk.start(24_000_000));

    try std.testing.expectEqual(12, seen_channel);
    try std.testing.expectEqual(0, seen_mode);
    try std.testing.expectEqual(0, seen_prescaler);
    try std.testing.expectEqual(9, seen_period);
    try std.testing.expectEqual(5, seen_duty_a);
    try std.testing.expectEqual(0, seen_duty_b);
    try std.testing.expectEqual(true, seen_auto_start);

    try std.testing.expectEqual(0, seen_pin);
    try std.testing.expectEqual(true, seen_output_enable);

    try std.testing.expectEqual(0x0501, seen_route_pin);
    try std.testing.expectEqual(0x02, seen_psel);
    try std.testing.expectEqualStrings("board.camera.xclk", std.mem.span(seen_owner));

    try std.testing.expectEqual(0x0501, seen_drive_pin);
    try std.testing.expectEqual(2, seen_dscr);
}

test "bring-up runs clock, timer, pin, route, drive in that order" {
    reset();
    _ = camera_xclk.start(24_000_000);
    try std.testing.expectEqual(5, steps);
    try std.testing.expectEqual(Step.clock, trace[0]);
    try std.testing.expectEqual(Step.init, trace[1]);
    try std.testing.expectEqual(Step.pin, trace[2]);
    try std.testing.expectEqual(Step.route, trace[3]);
    try std.testing.expectEqual(Step.drive, trace[4]);
}

test "each stage stops the sequence where it fails" {
    reset();
    init_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera_xclk.start(24_000_000));
    try std.testing.expectEqual(2, steps);

    reset();
    pin_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera_xclk.start(24_000_000));
    try std.testing.expectEqual(3, steps);

    reset();
    route_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera_xclk.start(24_000_000));
    try std.testing.expectEqual(4, steps);
}

test "the drive-strength result is the call's result" {
    reset();
    drive_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera_xclk.start(24_000_000));
    try std.testing.expectEqual(5, steps);
}
