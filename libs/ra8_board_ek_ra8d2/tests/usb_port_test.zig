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

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    gpio_pin = pin;
    gpio_level = init_level;
    gpio_calls += 1;
    return 0;
}

export fn ra8_board_usbhs_device_init() u32 {
    hs_device_calls += 1;
    return 0;
}

export fn ra8_board_usbhs_host_init() u32 {
    hs_host_calls += 1;
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
