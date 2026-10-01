//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freeing and handing over the system I2C bus.

const std = @import("std");
const io_exp_bus = @import("io_exp_bus");

const ok: u32 = 0;
const gpio_conflict: u32 = 0x205;

const level_low: u32 = 0;
const level_high: u32 = 1;

var out_init_calls: usize = 0;
var in_init_calls: usize = 0;
var release_calls: usize = 0;
var scl_pulses: usize = 0;
var routes: usize = 0;
var open_drains: usize = 0;
var last_route_pin: u16 = 0;
var last_od_port: u32 = 0xFF;
var last_od_pin: u32 = 0xFF;
var sda_level: u32 = level_low;
var sda_high_after: usize = 0xFF;
var out_init_fail_at: usize = 0xFF;
var in_init_fails: bool = false;
var route_fail_at: usize = 0xFF;
var od_fail_at: usize = 0xFF;

fn reset() void {
    out_init_calls = 0;
    in_init_calls = 0;
    release_calls = 0;
    scl_pulses = 0;
    routes = 0;
    open_drains = 0;
    sda_level = level_low;
    sda_high_after = 0xFF;
    out_init_fail_at = 0xFF;
    in_init_fails = false;
    route_fail_at = 0xFF;
    od_fail_at = 0xFF;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    _ = pin;
    _ = init_level;
    defer out_init_calls += 1;
    if (out_init_calls == out_init_fail_at) return gpio_conflict;
    return ok;
}

export fn ra8_gpio_input_init(pin: u16, pull: u32) u32 {
    _ = pin;
    _ = pull;
    in_init_calls += 1;
    return if (in_init_fails) gpio_conflict else ok;
}

export fn ra8_gpio_read(pin: u16, out_level: *u32) u32 {
    _ = pin;
    out_level.* = if (scl_pulses >= sda_high_after) level_high else sda_level;
    return ok;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    if (pin == io_exp_bus.scl and level == level_low) scl_pulses += 1;
    return ok;
}

export fn ra8_gpio_release(pin: u16) u32 {
    _ = pin;
    release_calls += 1;
    return ok;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = psel;
    _ = owner;
    defer routes += 1;
    last_route_pin = pin;
    if (routes == route_fail_at) return gpio_conflict;
    return ok;
}

export fn ra8_mpc_set_open_drain(port: u32, pin_index: u32, enable: bool) u32 {
    _ = enable;
    defer open_drains += 1;
    last_od_port = port;
    last_od_pin = pin_index;
    if (open_drains == od_fail_at) return gpio_conflict;
    return ok;
}

test "the bus is P512 and P511, not the I3C pair" {
    try std.testing.expectEqual(@as(u16, (5 << 8) | 12), io_exp_bus.scl);
    try std.testing.expectEqual(@as(u16, (5 << 8) | 11), io_exp_bus.sda);
}

test "the pull-up enables are P109 and P311" {
    try std.testing.expectEqual(@as(u16, (1 << 8) | 9), io_exp_bus.pullup_a);
    try std.testing.expectEqual(@as(u16, (3 << 8) | 11), io_exp_bus.pullup_b);
}

test "recovery on a free bus reports ok" {
    reset();
    sda_high_after = 0;
    try std.testing.expectEqual(ok, io_exp_bus.recover());
}

test "a bus already free is not clocked at all" {
    reset();
    sda_high_after = 0;
    _ = io_exp_bus.recover();
    try std.testing.expectEqual(@as(usize, 0), scl_pulses);
}

test "a wedged bus is clocked up to nine times" {
    reset();
    _ = io_exp_bus.recover();
    try std.testing.expectEqual(@as(usize, 9), scl_pulses);
}

test "clocking stops as soon as the peripheral lets go of SDA" {
    reset();
    sda_high_after = 3;
    _ = io_exp_bus.recover();
    try std.testing.expectEqual(@as(usize, 3), scl_pulses);
}

test "both pins are released when recovery ends" {
    reset();
    sda_high_after = 0;
    _ = io_exp_bus.recover();
    try std.testing.expect(release_calls >= 2);
}

test "a refused SCL claim aborts recovery" {
    reset();
    out_init_fail_at = 0;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.recover());
}

test "a refused SDA claim releases the SCL pin again" {
    reset();
    in_init_fails = true;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.recover());
    try std.testing.expectEqual(@as(usize, 1), release_calls);
}

test "both pull-up enables are driven" {
    reset();
    try std.testing.expectEqual(ok, io_exp_bus.enablePullups());
    try std.testing.expectEqual(@as(usize, 2), out_init_calls);
}

test "a refused first pull-up skips the second" {
    reset();
    out_init_fail_at = 0;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.enablePullups());
    try std.testing.expectEqual(@as(usize, 1), out_init_calls);
}

test "a refused second pull-up is reported" {
    reset();
    out_init_fail_at = 1;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.enablePullups());
}

test "routing claims both pins and sets open drain on both" {
    reset();
    try std.testing.expectEqual(ok, io_exp_bus.routePins());
    try std.testing.expectEqual(@as(usize, 2), routes);
    try std.testing.expectEqual(@as(usize, 2), open_drains);
}

test "the data pin is routed after the clock pin" {
    reset();
    _ = io_exp_bus.routePins();
    try std.testing.expectEqual(io_exp_bus.sda, last_route_pin);
}

test "open drain lands on port 5 pins 12 and 11" {
    reset();
    _ = io_exp_bus.routePins();
    try std.testing.expectEqual(@as(u32, 5), last_od_port);
    try std.testing.expectEqual(@as(u32, 11), last_od_pin);
}

test "a refused clock route stops before any open drain" {
    reset();
    route_fail_at = 0;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.routePins());
    try std.testing.expectEqual(@as(usize, 0), open_drains);
}

test "a refused clock open-drain stops before the data route" {
    reset();
    od_fail_at = 0;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.routePins());
    try std.testing.expectEqual(@as(usize, 1), routes);
}

test "a refused data open-drain is reported" {
    reset();
    od_fail_at = 1;
    try std.testing.expectEqual(gpio_conflict, io_exp_bus.routePins());
}
