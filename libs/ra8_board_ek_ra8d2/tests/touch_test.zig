//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GT911 bring-up: the point-cap policy, the bit rate solved against the live
//! PCLKA, and the order the four calls go out in.
//!
//! The `ra8_io` entry points are weak at link time. A test binary either
//! defines them or it does not, and it cannot be both, so these fakes stand
//! in for the linked case; the unlinked case is a link-time property and is
//! exercised by an app that omits `ra8_io`, not here.

const std = @import("std");
const touch = @import("touch");

const Err = struct {
    const ok: u32 = 0;
    const invalid_arg: u32 = 0x103;
    const not_found: u32 = 0x106;
    const not_initialized: u32 = 0x10F;
};

const I2cBusOps = extern struct {
    write: ?*const anyopaque,
    read: ?*const anyopaque,
    transfer: ?*const anyopaque,
    ctx: ?*anyopaque,
};

const I3cCfg = extern struct {
    mode: u8,
    bus_hz: u32,
    pclka_hz: u32,
};

const TouchCfg = extern struct {
    bus: I2cBusOps,
    target_7b: u8,
    irq_pin: u8,
    max_points: u8,
};

/// Call order, so the sequence itself is asserted and not just its parts.
const Step = enum { clock, i3c, bind, as_ops, open };
var steps: [8]Step = undefined;
var step_count: usize = 0;

fn note(step: Step) void {
    steps[step_count] = step;
    step_count += 1;
}

var clock_id: u32 = 0xFFFF;
var clock_hz: u32 = 125_000_000;
var clock_err: u32 = Err.ok;

var i3c_channel: u8 = 0xFF;
var i3c_cfg: I3cCfg = .{ .mode = 0xFF, .bus_hz = 0, .pclka_hz = 0 };
var i3c_err: u32 = Err.ok;

var bind_channel: u8 = 0xFF;
var bind_bus: ?*const anyopaque = null;
var bind_err: u32 = Err.ok;

var as_ops_bus: ?*const anyopaque = null;
var as_ops_err: u32 = Err.ok;

var opened: TouchCfg = undefined;
var open_err: u32 = Err.ok;

/// A recognisable ops table, so the one the driver receives can be shown to
/// be the one the bus published.
const marker: I2cBusOps = .{
    .write = @ptrFromInt(0xA000),
    .read = @ptrFromInt(0xB000),
    .transfer = @ptrFromInt(0xC000),
    .ctx = @ptrFromInt(0xD000),
};

fn reset() void {
    step_count = 0;
    clock_id = 0xFFFF;
    clock_hz = 125_000_000;
    clock_err = Err.ok;
    i3c_channel = 0xFF;
    i3c_cfg = .{ .mode = 0xFF, .bus_hz = 0, .pclka_hz = 0 };
    i3c_err = Err.ok;
    bind_channel = 0xFF;
    bind_bus = null;
    bind_err = Err.ok;
    as_ops_bus = null;
    as_ops_err = Err.ok;
    open_err = Err.ok;
    opened = .{
        .bus = .{ .write = null, .read = null, .transfer = null, .ctx = null },
        .target_7b = 0,
        .irq_pin = 0xFF,
        .max_points = 0xFF,
    };
}

export fn ra8_cgc_get_clock_hz(id: u32, out_hz: *u32) u32 {
    note(.clock);
    clock_id = id;
    if (clock_err != Err.ok) return clock_err;
    out_hz.* = clock_hz;
    return Err.ok;
}

export fn ra8_i3c_init(channel: u8, cfg: *const I3cCfg) u32 {
    note(.i3c);
    i3c_channel = channel;
    i3c_cfg = cfg.*;
    return i3c_err;
}

export fn ra8_io_i2c_bus_bind_i3c_compat(bus: *anyopaque, channel: u8) u32 {
    note(.bind);
    bind_bus = bus;
    bind_channel = channel;
    return bind_err;
}

export fn ra8_io_i2c_bus_as_ops(bus: *const anyopaque, out: *I2cBusOps) u32 {
    note(.as_ops);
    as_ops_bus = bus;
    if (as_ops_err != Err.ok) return as_ops_err;
    out.* = marker;
    return Err.ok;
}

export fn ra8_touch_open(cfg: *const TouchCfg) u32 {
    note(.open);
    opened = cfg.*;
    return open_err;
}

fn openWith(max_points: u8, irq_pin: u8) u32 {
    const cfg: touch.Cfg = .{ .max_points = max_points, .irq_pin = irq_pin };
    return touch.open(&cfg);
}

test "zero points selects the board default" {
    try std.testing.expectEqual(@as(?u8, 5), touch.resolvedPoints(0));
}

test "a point count inside the cap is kept" {
    try std.testing.expectEqual(@as(?u8, 1), touch.resolvedPoints(1));
    try std.testing.expectEqual(@as(?u8, 3), touch.resolvedPoints(3));
}

test "the cap itself is accepted" {
    try std.testing.expectEqual(@as(?u8, 5), touch.resolvedPoints(5));
}

test "a point count above the cap is refused, not clamped" {
    try std.testing.expectEqual(@as(?u8, null), touch.resolvedPoints(6));
    try std.testing.expectEqual(@as(?u8, null), touch.resolvedPoints(255));
}

test "too many points refuses before any hardware is touched" {
    reset();
    try std.testing.expectEqual(Err.invalid_arg, openWith(6, 4));
    try std.testing.expectEqual(@as(usize, 0), step_count);
}

test "a full bring-up reports ok" {
    reset();
    try std.testing.expectEqual(Err.ok, openWith(0, 4));
}

test "the four calls go out in order" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(usize, 5), step_count);
    try std.testing.expectEqual(Step.clock, steps[0]);
    try std.testing.expectEqual(Step.i3c, steps[1]);
    try std.testing.expectEqual(Step.bind, steps[2]);
    try std.testing.expectEqual(Step.as_ops, steps[3]);
    try std.testing.expectEqual(Step.open, steps[4]);
}

test "the bit rate is solved against PCLKA" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u32, 3), clock_id);
}

test "the live clock reading reaches the bus config" {
    reset();
    clock_hz = 125_000_000;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u32, 125_000_000), i3c_cfg.pclka_hz);
}

test "a different live clock is passed through, not a constant" {
    reset();
    clock_hz = 60_000_000;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u32, 60_000_000), i3c_cfg.pclka_hz);
}

test "the bus runs at fast mode" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u32, 400_000), i3c_cfg.bus_hz);
}

test "the channel comes up in I2C-compatibility mode" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u8, 1), i3c_cfg.mode);
}

test "channel 0 carries the GT911" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u8, 0), i3c_channel);
}

test "the bus is bound on the same channel it was brought up on" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(i3c_channel, bind_channel);
}

test "the module's own handle is what gets bound" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(?*const anyopaque, touch.boundBus()), bind_bus);
}

test "the ops are read back from the handle that was just bound" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(bind_bus, as_ops_bus);
}

test "the driver is handed the ops the bus published" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(marker.write, opened.bus.write);
    try std.testing.expectEqual(marker.read, opened.bus.read);
    try std.testing.expectEqual(marker.transfer, opened.bus.transfer);
    try std.testing.expectEqual(marker.ctx, opened.bus.ctx);
}

test "the GT911 default address is used" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u8, 0x5D), opened.target_7b);
}

test "the caller's IRQ pin reaches the driver untouched" {
    reset();
    _ = openWith(0, 11);
    try std.testing.expectEqual(@as(u8, 11), opened.irq_pin);
}

test "the polling sentinel is passed through like any other pin" {
    reset();
    _ = openWith(0, 32);
    try std.testing.expectEqual(@as(u8, 32), opened.irq_pin);
}

test "zero points reaches the driver as the board default" {
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(u8, 5), opened.max_points);
}

test "a requested point count reaches the driver as asked" {
    reset();
    _ = openWith(2, 4);
    try std.testing.expectEqual(@as(u8, 2), opened.max_points);
}

test "a failed clock read is returned" {
    reset();
    clock_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, openWith(0, 4));
}

test "a failed clock read stops before the bus comes up" {
    reset();
    clock_err = Err.not_found;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(usize, 1), step_count);
}

test "a failed bus init is returned" {
    reset();
    i3c_err = Err.not_initialized;
    try std.testing.expectEqual(Err.not_initialized, openWith(0, 4));
}

test "a failed bus init stops before the bind" {
    reset();
    i3c_err = Err.not_initialized;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(usize, 2), step_count);
}

test "a failed bind is returned" {
    reset();
    bind_err = Err.not_found;
    try std.testing.expectEqual(Err.not_found, openWith(0, 4));
}

test "a failed bind stops before the ops are read" {
    reset();
    bind_err = Err.not_found;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(usize, 3), step_count);
}

test "a failed ops read is returned" {
    reset();
    as_ops_err = Err.not_initialized;
    try std.testing.expectEqual(Err.not_initialized, openWith(0, 4));
}

test "a failed ops read stops before the driver opens" {
    reset();
    as_ops_err = Err.not_initialized;
    _ = openWith(0, 4);
    try std.testing.expectEqual(@as(usize, 4), step_count);
}

test "the driver's own refusal is returned unchanged" {
    reset();
    open_err = Err.invalid_arg;
    try std.testing.expectEqual(Err.invalid_arg, openWith(0, 4));
}

test "the bound handle is stable across opens" {
    reset();
    _ = openWith(0, 4);
    const first = bind_bus;
    reset();
    _ = openWith(0, 4);
    try std.testing.expectEqual(first, bind_bus);
}

test "the config the board hands the driver is laid out as C declares it" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(TouchCfg, "bus"));
    try std.testing.expectEqual(@sizeOf(I2cBusOps), @offsetOf(TouchCfg, "target_7b"));
    try std.testing.expectEqual(@sizeOf(I2cBusOps) + 1, @offsetOf(TouchCfg, "irq_pin"));
    try std.testing.expectEqual(@sizeOf(I2cBusOps) + 2, @offsetOf(TouchCfg, "max_points"));
}

test "the bus config is laid out as C declares it" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(I3cCfg, "mode"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(I3cCfg, "bus_hz"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(I3cCfg, "pclka_hz"));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(I3cCfg));
}

test "an application config is two bytes, as the header declares" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(touch.Cfg, "max_points"));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(touch.Cfg, "irq_pin"));
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(touch.Cfg));
}
