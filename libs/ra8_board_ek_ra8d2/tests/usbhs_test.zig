//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB-HS bring-up order, the PD07 role strap, and the bisect probes.

const std = @import("std");
const usbhs = @import("usbhs");

const ok: u32 = 0;
const gpio_conflict: u32 = 0x205;
const hw_init_failed: u32 = 0x201;
const nack: u32 = 0x407;

const Step = enum { pll, mstp, dev, host, role_pin };

var trace: [16]Step = undefined;
var traced: usize = 0;
var role_pin_level: u32 = 0xFF;
var role_pin_seen: u16 = 0;
var mstp_id: u16 = 0;
var dev_speed: u32 = 0xFF;
var host_speed: u32 = 0xFF;
var pll_fails: bool = false;
var mstp_fails: bool = false;
var role_fails: bool = false;
var dev_err: u32 = ok;
var i2c_nacks: bool = false;

fn note(step: Step) void {
    trace[traced] = step;
    traced += 1;
}

fn reset() void {
    traced = 0;
    role_pin_level = 0xFF;
    role_pin_seen = 0;
    mstp_id = 0;
    dev_speed = 0xFF;
    host_speed = 0xFF;
    pll_fails = false;
    mstp_fails = false;
    role_fails = false;
    dev_err = ok;
    i2c_nacks = false;
    usbhs.probe = 0;
    usbhs.role_probe = 0;
    usbhs.role_err = 0;
}

export fn ra8_cgc_usbhs_pll_enable() u32 {
    note(.pll);
    return if (pll_fails) hw_init_failed else ok;
}

export fn ra8_mstp_enable(id: u16) u32 {
    note(.mstp);
    mstp_id = id;
    return if (mstp_fails) hw_init_failed else ok;
}

export fn ra8_usb_device_init(speed: u32) u32 {
    note(.dev);
    dev_speed = speed;
    return dev_err;
}

export fn ra8_usb_host_init(speed: u32) u32 {
    note(.host);
    host_speed = speed;
    return ok;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    if (pin == usbhs.role_pin) {
        note(.role_pin);
        role_pin_seen = pin;
        role_pin_level = init_level;
        return if (role_fails) gpio_conflict else ok;
    }
    return ok;
}

// The U15 path underneath, which device mode calls best-effort.
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
    return ok;
}
export fn ra8_mpc_set_open_drain(port: u32, pin_index: u32, enable: bool) u32 {
    _ = port;
    _ = pin_index;
    _ = enable;
    return ok;
}
export fn ra8_i2c_init(channel: u8, cfg: *const extern struct { bus_hz: u32, pclkb_hz: u32 }) u32 {
    _ = channel;
    _ = cfg;
    return ok;
}
export fn ra8_i2c_write(channel: u8, addr_7b: u8, data: [*]const u8, len: usize, send_stop: bool) u32 {
    _ = channel;
    _ = addr_7b;
    _ = data;
    _ = len;
    _ = send_stop;
    return if (i2c_nacks) nack else ok;
}

test "the role pin is PD07" {
    try std.testing.expectEqual(@as(u16, (13 << 8) | 7), usbhs.role_pin);
}

test "the role strap drives PD07 low for device mode" {
    reset();
    try std.testing.expectEqual(ok, usbhs.roleSelectDevice());
    try std.testing.expectEqual(@as(u32, 0), role_pin_level);
}

test "a successful strap ends at the success step" {
    reset();
    _ = usbhs.roleSelectDevice();
    try std.testing.expectEqual(usbhs.RoleStep.success, usbhs.role_probe);
}

test "a refused strap records the error and stops at post-init" {
    reset();
    role_fails = true;
    try std.testing.expectEqual(gpio_conflict, usbhs.roleSelectDevice());
    try std.testing.expectEqual(gpio_conflict, usbhs.role_err);
    try std.testing.expectEqual(usbhs.RoleStep.post_init, usbhs.role_probe);
}

test "the clock comes up before the gate is opened" {
    reset();
    try std.testing.expectEqual(ok, usbhs.clockAndMstp());
    try std.testing.expectEqualSlices(Step, &.{ .pll, .mstp }, trace[0..traced]);
}

test "the gate opened is MSTPB12" {
    reset();
    _ = usbhs.clockAndMstp();
    try std.testing.expectEqual(@as(u16, (1 << 8) | 12), mstp_id);
}

test "a failed PHY clock stops before the gate" {
    reset();
    pll_fails = true;
    try std.testing.expectEqual(hw_init_failed, usbhs.clockAndMstp());
    try std.testing.expectEqualSlices(Step, &.{.pll}, trace[0..traced]);
}

test "a failed PHY clock leaves the probe at the clock step" {
    reset();
    pll_fails = true;
    _ = usbhs.clockAndMstp();
    try std.testing.expectEqual(usbhs.Step.pre_pll_enable, usbhs.probe);
}

test "device bring-up straps the role, then clocks, gates and starts the controller" {
    reset();
    try std.testing.expectEqual(ok, usbhs.deviceInit());
    try std.testing.expectEqualSlices(Step, &.{ .role_pin, .pll, .mstp, .dev }, trace[0..traced]);
}

test "device bring-up asks for high speed" {
    reset();
    _ = usbhs.deviceInit();
    try std.testing.expectEqual(@as(u32, 1), dev_speed);
}

test "device bring-up ends past the controller start" {
    reset();
    _ = usbhs.deviceInit();
    try std.testing.expectEqual(usbhs.Step.post_usb_dev_init, usbhs.probe);
}

test "a refused strap aborts device bring-up before the clock" {
    reset();
    role_fails = true;
    try std.testing.expectEqual(gpio_conflict, usbhs.deviceInit());
    try std.testing.expectEqualSlices(Step, &.{.role_pin}, trace[0..traced]);
}

test "a U15 NACK is survivable, because PD07 already strapped the role" {
    reset();
    i2c_nacks = true;
    try std.testing.expectEqual(ok, usbhs.deviceInit());
}

test "a failed gate aborts before the controller starts" {
    reset();
    mstp_fails = true;
    try std.testing.expectEqual(hw_init_failed, usbhs.deviceInit());
    try std.testing.expectEqualSlices(Step, &.{ .role_pin, .pll, .mstp }, trace[0..traced]);
}

test "the controller's own error is passed through" {
    reset();
    dev_err = 0x321;
    try std.testing.expectEqual(@as(u32, 0x321), usbhs.deviceInit());
}

test "host bring-up clocks and gates, then starts the host controller" {
    reset();
    try std.testing.expectEqual(ok, usbhs.hostInit());
    try std.testing.expectEqualSlices(Step, &.{ .pll, .mstp, .host }, trace[0..traced]);
}

test "host bring-up never touches the role strap" {
    reset();
    _ = usbhs.hostInit();
    try std.testing.expectEqual(@as(u16, 0), role_pin_seen);
}

test "host bring-up asks for high speed too" {
    reset();
    _ = usbhs.hostInit();
    try std.testing.expectEqual(@as(u32, 1), host_speed);
}

test "a failed clock aborts host bring-up" {
    reset();
    pll_fails = true;
    try std.testing.expectEqual(hw_init_failed, usbhs.hostInit());
    try std.testing.expectEqualSlices(Step, &.{.pll}, trace[0..traced]);
}
