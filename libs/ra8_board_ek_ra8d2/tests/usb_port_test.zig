//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB connector bring-up against a fake pin seam. The case that matters is
//! VBUSEN: it must be driven as a GPIO, never routed to the peripheral
//! function, or device enumeration never completes.

const std = @import("std");
const usb_port = @import("usb_port");

const Routed = struct { pin: u16, psel: u32 };

var routed: [8]Routed = undefined;
var routed_len: usize = 0;
var route_err: u32 = 0;
var gpio_pin: u16 = 0;
var gpio_level: u32 = 0xFFFF;
var gpio_calls: usize = 0;
var hs_device_calls: usize = 0;
var hs_host_calls: usize = 0;

fn reset() void {
    routed_len = 0;
    route_err = 0;
    gpio_pin = 0;
    gpio_level = 0xFFFF;
    gpio_calls = 0;
    hs_device_calls = 0;
    hs_host_calls = 0;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = owner;
    if (route_err != 0) return route_err;
    routed[routed_len] = .{ .pin = pin, .psel = psel };
    routed_len += 1;
    return 0;
}

/// PD07, the USB-HS role strap. It belongs to the high-speed controller, not
/// to this connector, so it is deliberately kept out of the pin bookkeeping
/// these tests assert on.
const hs_role_pin: u16 = (13 << 8) | 7;

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    if (pin == hs_role_pin) return 0;
    gpio_pin = pin;
    gpio_level = init_level;
    gpio_calls += 1;
    return 0;
}

// The high-speed controller underneath, which this connector only dispatches
// to. Its own ordering is covered by the usbhs suite.
export fn ra8_cgc_usbhs_pll_enable() u32 {
    return 0;
}

export fn ra8_mstp_enable(id: u16) u32 {
    _ = id;
    return 0;
}

export fn ra8_usb_device_init(speed: u32) u32 {
    _ = speed;
    hs_device_calls += 1;
    return 0;
}

export fn ra8_usb_host_init(speed: u32) u32 {
    _ = speed;
    hs_host_calls += 1;
    return 0;
}

// The U15 expander the high-speed path pokes best-effort on its way through.
export fn ra8_gpio_input_init(pin: u16, pull: u32) u32 {
    _ = pin;
    _ = pull;
    return 0;
}

export fn ra8_gpio_read(pin: u16, out_level: *u32) u32 {
    _ = pin;
    out_level.* = 1;
    return 0;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    _ = pin;
    _ = level;
    return 0;
}

export fn ra8_gpio_release(pin: u16) u32 {
    _ = pin;
    return 0;
}

export fn ra8_mpc_set_open_drain(port: u32, pin_index: u32, enable: bool) u32 {
    _ = port;
    _ = pin_index;
    _ = enable;
    return 0;
}

export fn ra8_i2c_init(channel: u8, cfg: *const extern struct { bus_hz: u32, pclkb_hz: u32 }) u32 {
    _ = channel;
    _ = cfg;
    return 0;
}

export fn ra8_i2c_write(channel: u8, addr_7b: u8, data: [*]const u8, len: usize, send_stop: bool) u32 {
    _ = channel;
    _ = addr_7b;
    _ = data;
    _ = len;
    _ = send_stop;
    return 0;
}

test "an unknown role is refused before anything is touched" {
    reset();
    try std.testing.expectEqual(usb_port.Err.invalid_arg, usb_port.init(usb_port.Port.fs, 7));
    try std.testing.expectEqual(@as(usize, 0), routed_len);
    try std.testing.expectEqual(@as(usize, 0), gpio_calls);
}

test "an unknown port is refused" {
    reset();
    try std.testing.expectEqual(usb_port.Err.invalid_arg, usb_port.init(9, usb_port.Role.device));
}

test "full speed routes exactly the three peripheral pins" {
    reset();
    try std.testing.expectEqual(usb_port.Err.ok, usb_port.init(usb_port.Port.fs, usb_port.Role.device));
    try std.testing.expectEqual(@as(usize, 3), routed_len);
    try std.testing.expectEqual(usb_port.FsPin.vbus, routed[0].pin);
    try std.testing.expectEqual(usb_port.FsPin.dp, routed[1].pin);
    try std.testing.expectEqual(usb_port.FsPin.dm, routed[2].pin);
}

test "VBUSEN is never routed to the peripheral function" {
    reset();
    _ = usb_port.init(usb_port.Port.fs, usb_port.Role.host);
    for (routed[0..routed_len]) |entry| {
        try std.testing.expect(entry.pin != usb_port.FsPin.vbusen);
    }
    try std.testing.expectEqual(usb_port.FsPin.vbusen, gpio_pin);
}

test "device straps VBUSEN low, host drives it high" {
    reset();
    _ = usb_port.init(usb_port.Port.fs, usb_port.Role.device);
    try std.testing.expectEqual(@as(u32, 0), gpio_level);
    reset();
    _ = usb_port.init(usb_port.Port.fs, usb_port.Role.host);
    try std.testing.expectEqual(@as(u32, 1), gpio_level);
}

test "a routing failure stops before the role is strapped" {
    reset();
    route_err = 0x205;
    try std.testing.expectEqual(@as(u32, 0x205), usb_port.init(usb_port.Port.fs, usb_port.Role.device));
    try std.testing.expectEqual(@as(usize, 0), gpio_calls);
}

test "high speed defers to the controller helpers" {
    reset();
    try std.testing.expectEqual(usb_port.Err.ok, usb_port.init(usb_port.Port.hs, usb_port.Role.device));
    try std.testing.expectEqual(@as(usize, 1), hs_device_calls);
    reset();
    try std.testing.expectEqual(usb_port.Err.ok, usb_port.init(usb_port.Port.hs, usb_port.Role.host));
    try std.testing.expectEqual(@as(usize, 1), hs_host_calls);
}

test "high speed touches no pin itself" {
    reset();
    _ = usb_port.init(usb_port.Port.hs, usb_port.Role.host);
    try std.testing.expectEqual(@as(usize, 0), routed_len);
    try std.testing.expectEqual(@as(usize, 0), gpio_calls);
}
