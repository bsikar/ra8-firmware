//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The U15 bring-up order, the register writes, and the SW4 layouts.

const std = @import("std");
const io_expander = @import("io_expander");

const ok: u32 = 0;
const nack: u32 = 0x407;
const gpio_conflict: u32 = 0x205;
const hw_init_failed: u32 = 0x201;

const addr: u8 = 0x43;

var writes: usize = 0;
var write_regs: [8]u8 = undefined;
var write_vals: [8]u8 = undefined;
var write_addrs: [8]u8 = undefined;
var write_stops: [8]bool = undefined;
var write_lens: [8]usize = undefined;
var write_fail_at: usize = 0xFF;

var i2c_inits: usize = 0;
var i2c_init_fails: bool = false;
var i2c_channel: u8 = 0xFF;
var i2c_bus_hz: u32 = 0;
var i2c_pclkb_hz: u32 = 0;

var gpio_calls: usize = 0;
var routes: usize = 0;
var recover_fails: bool = false;
var pullup_fails: bool = false;
var route_fails: bool = false;

fn reset() void {
    writes = 0;
    write_fail_at = 0xFF;
    i2c_inits = 0;
    i2c_init_fails = false;
    i2c_channel = 0xFF;
    gpio_calls = 0;
    routes = 0;
    recover_fails = false;
    pullup_fails = false;
    route_fails = false;
    io_expander.probe = 0;
}

export fn ra8_i2c_write(channel: u8, addr_7b: u8, data: [*]const u8, len: usize, send_stop: bool) u32 {
    _ = channel;
    defer writes += 1;
    if (writes == write_fail_at) return nack;
    write_addrs[writes] = addr_7b;
    write_regs[writes] = data[0];
    write_vals[writes] = data[1];
    write_lens[writes] = len;
    write_stops[writes] = send_stop;
    return ok;
}

export fn ra8_i2c_init(channel: u8, cfg: *const extern struct { bus_hz: u32, pclkb_hz: u32 }) u32 {
    i2c_inits += 1;
    i2c_channel = channel;
    i2c_bus_hz = cfg.bus_hz;
    i2c_pclkb_hz = cfg.pclkb_hz;
    return if (i2c_init_fails) hw_init_failed else ok;
}

// The bus layer underneath, stubbed at the GPIO/PFS boundary it sits on.
export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    _ = init_level;
    gpio_calls += 1;
    // The first two output claims belong to recovery, later ones to the pull-ups.
    if (recover_fails and gpio_calls == 1) return gpio_conflict;
    if (pullup_fails and pin == (1 << 8 | 9)) return gpio_conflict;
    return ok;
}
export fn ra8_gpio_input_init(pin: u16, pull: u32) u32 {
    _ = pin;
    _ = pull;
    return ok;
}
export fn ra8_gpio_read(pin: u16, out_level: *u32) u32 {
    _ = pin;
    out_level.* = 1;
    return ok;
}
export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    _ = pin;
    _ = level;
    return ok;
}
export fn ra8_gpio_release(pin: u16) u32 {
    _ = pin;
    return ok;
}
export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = pin;
    _ = psel;
    _ = owner;
    routes += 1;
    return if (route_fails) gpio_conflict else ok;
}
export fn ra8_mpc_set_open_drain(port: u32, pin_index: u32, enable: bool) u32 {
    _ = port;
    _ = pin_index;
    _ = enable;
    return ok;
}

test "a clean apply reports ok" {
    reset();
    try std.testing.expectEqual(ok, io_expander.applyMask(0xF2, 0xFF));
}

test "a clean apply ends at the success step" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(io_expander.Step.success, io_expander.probe);
}

test "U15 takes exactly three register writes" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(@as(usize, 3), writes);
}

test "the writes go out in the FSP order: output, Hi-Z, direction" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(@as(u8, 0x05), write_regs[0]);
    try std.testing.expectEqual(@as(u8, 0x07), write_regs[1]);
    try std.testing.expectEqual(@as(u8, 0x03), write_regs[2]);
}

test "the output latch carries the requested byte" {
    reset();
    _ = io_expander.applyMask(0x5A, 0xFF);
    try std.testing.expectEqual(@as(u8, 0x5A), write_vals[0]);
}

test "Hi-Z is always cleared" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(@as(u8, 0x00), write_vals[1]);
}

test "the direction register carries the requested mask" {
    reset();
    _ = io_expander.applyMask(0xF2, 0x20);
    try std.testing.expectEqual(@as(u8, 0x20), write_vals[2]);
}

test "every write addresses U15 at 0x43" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    for (write_addrs[0..3]) |a| try std.testing.expectEqual(addr, a);
}

test "every write is a complete two-byte transaction closed with a STOP" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    for (write_lens[0..3]) |l| try std.testing.expectEqual(@as(usize, 2), l);
    for (write_stops[0..3]) |s| try std.testing.expect(s);
}

test "RIIC1 comes up at 100 kHz against a 62.5 MHz PCLKB" {
    reset();
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(@as(usize, 1), i2c_inits);
    try std.testing.expectEqual(@as(u8, 1), i2c_channel);
    try std.testing.expectEqual(@as(u32, 100_000), i2c_bus_hz);
    try std.testing.expectEqual(@as(u32, 62_500_000), i2c_pclkb_hz);
}

test "the bus is brought up before any register write" {
    reset();
    i2c_init_fails = true;
    _ = io_expander.applyMask(0xF2, 0xFF);
    try std.testing.expectEqual(@as(usize, 0), writes);
}

test "a failed bus bring-up is reported and stops at the init step" {
    reset();
    i2c_init_fails = true;
    try std.testing.expectEqual(hw_init_failed, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(io_expander.Step.pre_init, io_expander.probe);
}

test "a refused route stops before the bus is initialized" {
    reset();
    route_fails = true;
    try std.testing.expectEqual(gpio_conflict, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(@as(usize, 0), i2c_inits);
    try std.testing.expectEqual(io_expander.Step.pre_pfs, io_expander.probe);
}

test "a failed recovery stops before anything else" {
    reset();
    recover_fails = true;
    try std.testing.expectEqual(gpio_conflict, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(@as(usize, 0), routes);
}

test "a NACK on the output latch stops the sequence there" {
    reset();
    write_fail_at = 0;
    try std.testing.expectEqual(nack, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(io_expander.Step.pre_write_out, io_expander.probe);
}

test "a NACK on the Hi-Z write stops before the direction write" {
    reset();
    write_fail_at = 1;
    try std.testing.expectEqual(nack, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(io_expander.Step.pre_write_hiz, io_expander.probe);
}

test "a NACK on the direction write is reported" {
    reset();
    write_fail_at = 2;
    try std.testing.expectEqual(nack, io_expander.applyMask(0xF2, 0xFF));
    try std.testing.expectEqual(io_expander.Step.pre_write_dir, io_expander.probe);
}

test "device mode drives every SW4 channel to its OFF default" {
    reset();
    try std.testing.expectEqual(ok, io_expander.setUsbhsDeviceMode());
    try std.testing.expectEqual(@as(u8, 0xFF), write_vals[0]);
    try std.testing.expectEqual(@as(u8, 0xFF), write_vals[2]);
}

test "host mode latches 0x72" {
    reset();
    try std.testing.expectEqual(ok, io_expander.setUsbhsHostMode());
    try std.testing.expectEqual(@as(u8, 0x72), write_vals[0]);
}

test "the project defaults latch 0xF2" {
    reset();
    try std.testing.expectEqual(ok, io_expander.applyProjectSw4Defaults());
    try std.testing.expectEqual(@as(u8, 0xF2), write_vals[0]);
}

test "the octo-SPI layout latches 0xF8" {
    reset();
    try std.testing.expectEqual(ok, io_expander.setOctospiActive());
    try std.testing.expectEqual(@as(u8, 0xF8), write_vals[0]);
}

test "applySw4 drives every pin as an output" {
    reset();
    try std.testing.expectEqual(ok, io_expander.applySw4(0x0F));
    try std.testing.expectEqual(@as(u8, 0x0F), write_vals[0]);
    try std.testing.expectEqual(@as(u8, 0xFF), write_vals[2]);
}

test "a partial mask releases the rest of SW4 to the physical switches" {
    reset();
    _ = io_expander.applyMask(0xDF, 0x20);
    try std.testing.expectEqual(@as(u8, 0x20), write_vals[2]);
}
