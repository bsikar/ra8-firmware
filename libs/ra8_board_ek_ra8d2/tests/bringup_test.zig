//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The board prologue against a fake substrate. What the suite is really
//! holding down is the order: rates read back live, console before LEDs, and
//! interrupts unmasked last.

const std = @import("std");
const bringup = @import("bringup");

const Step = enum { clocks, mstp, cpuclk0, pclka, time, console, led, isr };

var log: [16]Step = undefined;
var log_len: usize = 0;
var fail_at: ?Step = null;
var led_order: [4]u32 = undefined;
var led_count: usize = 0;
var console_baud: u32 = 0;

fn reset() void {
    log_len = 0;
    fail_at = null;
    led_count = 0;
    console_baud = 0;
}

fn note(step: Step) u32 {
    log[log_len] = step;
    log_len += 1;
    if (fail_at) |want| {
        if (want == step) return 0x103;
    }
    return 0;
}

fn sawStep(step: Step) bool {
    for (log[0..log_len]) |entry| {
        if (entry == step) return true;
    }
    return false;
}

export fn ra8_board_clocks_init(out_rates: *bringup.Rates) u32 {
    out_rates.* = .{ .cpuclk0_hz = 1, .pclka_hz = 1 };
    return note(.clocks);
}

export fn ra8_mstp_init() u32 {
    return note(.mstp);
}

export fn ra8_cgc_get_clock_hz(id: u32, out_hz: *u32) u32 {
    if (id == 0) {
        out_hz.* = 480_000_000;
        return note(.cpuclk0);
    }
    out_hz.* = 125_000_000;
    return note(.pclka);
}

export fn ra8_time_init(cpu_hz: u32) u32 {
    _ = cpu_hz;
    return note(.time);
}

export fn ra8_board_uart_console_init(baud: u32) u32 {
    console_baud = baud;
    return note(.console);
}

export fn ra8_board_led_init(led: u32) u32 {
    led_order[led_count] = led;
    led_count += 1;
    return note(.led);
}

export fn ra8_isr_globals_enable() void {
    _ = note(.isr);
}

test "a null cfg or out is refused" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0, .enable_interrupts = false };
    try std.testing.expectEqual(bringup.Err.null_ptr, bringup.run(null, &out));
    try std.testing.expectEqual(bringup.Err.null_ptr, bringup.run(&cfg, null));
    try std.testing.expectEqual(@as(usize, 0), log_len);
}

test "a mask bit outside the three LEDs is refused" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0x8, .enable_interrupts = false };
    try std.testing.expectEqual(bringup.Err.invalid_arg, bringup.run(&cfg, &out));
    try std.testing.expectEqual(@as(usize, 0), log_len);
}

test "the rates handed back are the ones read back live" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0, .enable_interrupts = false };
    try std.testing.expectEqual(bringup.Err.ok, bringup.run(&cfg, &out));
    try std.testing.expectEqual(@as(u32, 480_000_000), out.cpuclk0_hz);
    try std.testing.expectEqual(@as(u32, 125_000_000), out.pclka_hz);
}

test "the substrate runs clocks, module stop, rates, then the timebase" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0, .enable_interrupts = false };
    _ = bringup.run(&cfg, &out);
    try std.testing.expectEqual(Step.clocks, log[0]);
    try std.testing.expectEqual(Step.mstp, log[1]);
    try std.testing.expectEqual(Step.cpuclk0, log[2]);
    try std.testing.expectEqual(Step.pclka, log[3]);
    try std.testing.expectEqual(Step.time, log[4]);
}

test "baud zero brings no console up" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0, .enable_interrupts = false };
    _ = bringup.run(&cfg, &out);
    try std.testing.expect(!sawStep(.console));
}

test "a baud brings the console up with it" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{ .console_baud = 115_200, .leds_mask = 0, .enable_interrupts = false };
    _ = bringup.run(&cfg, &out);
    try std.testing.expect(sawStep(.console));
    try std.testing.expectEqual(@as(u32, 115_200), console_baud);
}

test "only the named LEDs come up, in id order" {
    reset();
    var out: bringup.Rates = undefined;
    const cfg: bringup.Cfg = .{
        .console_baud = 0,
        .leds_mask = bringup.Leds.led3 | bringup.Leds.led1,
        .enable_interrupts = false,
    };
    _ = bringup.run(&cfg, &out);
    try std.testing.expectEqual(@as(usize, 2), led_count);
    try std.testing.expectEqual(@as(u32, 0), led_order[0]);
    try std.testing.expectEqual(@as(u32, 2), led_order[1]);
}

test "interrupts are unmasked last, and only when asked" {
    reset();
    var out: bringup.Rates = undefined;
    const quiet: bringup.Cfg = .{ .console_baud = 0, .leds_mask = 0, .enable_interrupts = false };
    _ = bringup.run(&quiet, &out);
    try std.testing.expect(!sawStep(.isr));

    reset();
    const loud: bringup.Cfg = .{
        .console_baud = 115_200,
        .leds_mask = bringup.Leds.all,
        .enable_interrupts = true,
    };
    _ = bringup.run(&loud, &out);
    try std.testing.expectEqual(Step.isr, log[log_len - 1]);
}

test "a failing step stops the prologue and leaves out untouched" {
    reset();
    fail_at = .mstp;
    var out: bringup.Rates = .{ .cpuclk0_hz = 7, .pclka_hz = 7 };
    const cfg: bringup.Cfg = .{ .console_baud = 115_200, .leds_mask = bringup.Leds.all, .enable_interrupts = true };
    try std.testing.expectEqual(@as(u32, 0x103), bringup.run(&cfg, &out));
    try std.testing.expectEqual(@as(u32, 7), out.cpuclk0_hz);
    try std.testing.expect(!sawStep(.console));
    try std.testing.expectEqual(@as(usize, 0), led_count);
}
