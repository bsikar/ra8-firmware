//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J35 adapter: the U15 latch, the CEU route table, the reset pulse, and
//! the SCCB bus binding.
//!
//! The two `ra8_io` entry points are weak at link time. A test binary either
//! defines them or it does not, and it cannot be both, so these fakes stand
//! in for the linked case; the unlinked case is a link-time property and is
//! exercised by an app that omits `ra8_io`, not here.

const std = @import("std");
const camera = @import("camera");

const Err = struct {
    const ok: u32 = 0;
    const not_found: u32 = 0x106;
};

var routed_pins: [16]u16 = undefined;
var routed: usize = 0;
var routed_psel: u32 = 0xFFFF;
var routed_owner: [*:0]const u8 = "";
var route_fail_at: usize = 0xFF;

var gpio_init_pin: u16 = 0;
var gpio_init_level: u32 = 0xFF;
var gpio_init_err: u32 = Err.ok;
var gpio_write_pin: u16 = 0;
var gpio_write_level: u32 = 0xFF;
var gpio_write_err: u32 = Err.ok;

var delays: [8]u32 = undefined;
var delay_count: usize = 0;

var latch_output: u8 = 0;
var latch_mask: u8 = 0;
var latch_err: u32 = Err.ok;

var bind_channel: u8 = 0xFF;
var bind_bus: ?*const anyopaque = null;
var bind_err: u32 = Err.ok;
var as_ops_bus: ?*const anyopaque = null;
var as_ops_err: u32 = Err.ok;
var as_ops_called: bool = false;

fn reset() void {
    routed = 0;
    route_fail_at = 0xFF;
    gpio_init_err = Err.ok;
    gpio_write_err = Err.ok;
    gpio_init_level = 0xFF;
    gpio_write_level = 0xFF;
    delay_count = 0;
    latch_err = Err.ok;
    bind_err = Err.ok;
    as_ops_err = Err.ok;
    as_ops_called = false;
    bind_channel = 0xFF;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    if (routed == route_fail_at) return Err.not_found;
    routed_pins[routed] = pin;
    routed += 1;
    routed_psel = psel;
    routed_owner = owner;
    return Err.ok;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    gpio_init_pin = pin;
    gpio_init_level = init_level;
    return gpio_init_err;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    gpio_write_pin = pin;
    gpio_write_level = level;
    return gpio_write_err;
}

export fn ra8_delay_ms(milliseconds: u32) void {
    delays[delay_count] = milliseconds;
    delay_count += 1;
}

export fn ra8_board_io_expander_apply_sw4_mask(output_byte: u8, output_mask: u8) u32 {
    latch_output = output_byte;
    latch_mask = output_mask;
    return latch_err;
}

export fn ra8_io_i2c_bus_bind_riic(bus: *anyopaque, channel: u8) callconv(.c) u32 {
    bind_bus = bus;
    bind_channel = channel;
    return bind_err;
}

export fn ra8_io_i2c_bus_as_ops(bus: *const anyopaque, out: *anyopaque) callconv(.c) u32 {
    _ = out;
    as_ops_bus = bus;
    as_ops_called = true;
    return as_ops_err;
}

test "the latch is thrown with the SW4-6 override bit only" {
    reset();
    try std.testing.expectEqual(Err.ok, camera.selectParallel());
    try std.testing.expectEqual(0xDF, latch_output);
    try std.testing.expectEqual(0x20, latch_mask);
}

test "a refused latch is returned as-is" {
    reset();
    latch_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera.selectParallel());
}

test "every CEU pin is routed, in map order, to the CEU function" {
    reset();
    try std.testing.expectEqual(Err.ok, camera.routeParallelPins());
    try std.testing.expectEqual(11, routed);

    const want = [_]u16{
        0x0400, 0x0902, 0x0405, 0x0406, 0x0700, 0x0701,
        0x0702, 0x0703, 0x0B02, 0x0B03, 0x0B04,
    };
    try std.testing.expectEqualSlices(u16, &want, routed_pins[0..routed]);
    try std.testing.expectEqual(0x0F, routed_psel);
    try std.testing.expectEqualStrings("board.camera.ceu", std.mem.span(routed_owner));
}

test "routing stops at the pin that actually clashed" {
    reset();
    route_fail_at = 4;
    try std.testing.expectEqual(Err.not_found, camera.routeParallelPins());
    try std.testing.expectEqual(4, routed);
}

test "neither XCLK nor reset is in the CEU route table" {
    reset();
    _ = camera.routeParallelPins();
    for (routed_pins[0..routed]) |pin| {
        try std.testing.expect(pin != 0x0501);
        try std.testing.expect(pin != 0x0709);
    }
}

test "reset drives P709 low, holds, releases, holds again" {
    reset();
    try std.testing.expectEqual(Err.ok, camera.reset());
    try std.testing.expectEqual(0x0709, gpio_init_pin);
    try std.testing.expectEqual(0, gpio_init_level);
    try std.testing.expectEqual(0x0709, gpio_write_pin);
    try std.testing.expectEqual(1, gpio_write_level);
    try std.testing.expectEqual(2, delay_count);
    try std.testing.expectEqual(20, delays[0]);
    try std.testing.expectEqual(20, delays[1]);
}

test "a failed reset drive leaves the line unheld" {
    reset();
    gpio_init_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera.reset());
    try std.testing.expectEqual(0, delay_count);

    reset();
    gpio_write_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera.reset());
    try std.testing.expectEqual(1, delay_count);
}

test "the delay hook ignores its context and forwards the interval" {
    reset();
    camera.delayMs(null, 7);
    try std.testing.expectEqual(1, delay_count);
    try std.testing.expectEqual(7, delays[0]);
}

test "the SCCB bus binds RIIC1 and is published through the same storage" {
    reset();
    var ops: camera.I2cBusOps = undefined;
    try std.testing.expectEqual(Err.ok, camera.i2cOps(&ops));
    try std.testing.expectEqual(1, bind_channel);
    try std.testing.expect(as_ops_called);
    try std.testing.expectEqual(@intFromPtr(camera.boundBus()), @intFromPtr(bind_bus.?));
    try std.testing.expectEqual(@intFromPtr(bind_bus.?), @intFromPtr(as_ops_bus.?));
}

test "a failed bind is returned without publishing anything" {
    reset();
    var ops: camera.I2cBusOps = undefined;
    bind_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera.i2cOps(&ops));
    try std.testing.expect(!as_ops_called);
}

test "the publish result is the call's result" {
    reset();
    var ops: camera.I2cBusOps = undefined;
    as_ops_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, camera.i2cOps(&ops));
    try std.testing.expect(as_ops_called);
}
