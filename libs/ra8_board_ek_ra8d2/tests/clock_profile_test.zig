//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The board clock profile: which chip instance each board-level module index
//! lands on. The scattered SCI channels are the reason the table is explicit,
//! so the suite pins all four of them.

const std = @import("std");
const clock_profile = @import("clock_profile");

const Kind = clock_profile.Kind;
const Module = clock_profile.Module;

var bind_calls: usize = 0;
var bound_iface: ?*const anyopaque = null;
var chip_rate_calls: usize = 0;
var chip_gate_calls: usize = 0;
var last_chip: Module = .{ .kind = 0, .index = 0 };

fn chipRate(ctx: ?*anyopaque, module: Module, out_hz: *u32) callconv(.c) u32 {
    _ = ctx;
    last_chip = module;
    chip_rate_calls += 1;
    out_hz.* = 125_000_000;
    return 0;
}

fn chipGate(ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32 {
    _ = ctx;
    _ = on;
    last_chip = module;
    chip_gate_calls += 1;
    return 0;
}

fn chipHas(ctx: ?*anyopaque, module: Module, out_present: *bool) callconv(.c) u32 {
    _ = ctx;
    _ = module;
    out_present.* = true;
    return 0;
}

const chip_iface: clock_profile.Iface = .{
    .rate_for = chipRate,
    .set_gate = chipGate,
    .has_module = chipHas,
};

export fn fw_clock_ra8_iface() *const clock_profile.Iface {
    return &chip_iface;
}

export fn fw_clock_bind(clk: *clock_profile.FwClock, iface: *const clock_profile.Iface, ctx: ?*anyopaque) u32 {
    bind_calls += 1;
    bound_iface = @ptrCast(iface);
    clk.* = .{ .iface = iface, .ctx = ctx, .bound = true };
    return 0;
}

test "a null out is refused" {
    const module: Module = .{ .kind = Kind.uart, .index = 0 };
    try std.testing.expectEqual(clock_profile.Err.invalid_arg, clock_profile.toChip(module, null));
}

test "a kind outside the table is not found" {
    var chip: Module = undefined;
    const module: Module = .{ .kind = 200, .index = 0 };
    try std.testing.expectEqual(clock_profile.Err.not_found, clock_profile.toChip(module, &chip));
}

test "an index past what the board wires is not found" {
    var chip: Module = undefined;
    const module: Module = .{ .kind = Kind.uart, .index = 4 };
    try std.testing.expectEqual(clock_profile.Err.not_found, clock_profile.toChip(module, &chip));
}

test "the four wired UARTs are SCI 0, 2, 7 and 8" {
    const want = [_]u8{ 0, 2, 7, 8 };
    for (want, 0..) |chip_index, board_index| {
        var chip: Module = undefined;
        const module: Module = .{ .kind = Kind.uart, .index = @intCast(board_index) };
        try std.testing.expectEqual(clock_profile.Err.ok, clock_profile.toChip(module, &chip));
        try std.testing.expectEqual(chip_index, chip.index);
        try std.testing.expectEqual(Kind.uart, chip.kind);
    }
}

test "the one wired I2C bus is RIIC1, not the I3C touch bus" {
    var chip: Module = undefined;
    const module: Module = .{ .kind = Kind.i2c, .index = 0 };
    try std.testing.expectEqual(clock_profile.Err.ok, clock_profile.toChip(module, &chip));
    try std.testing.expectEqual(@as(u8, 1), chip.index);

    const second: Module = .{ .kind = Kind.i2c, .index = 1 };
    try std.testing.expectEqual(clock_profile.Err.not_found, clock_profile.toChip(second, &chip));
}

test "single-instance blocks pass their numbering through" {
    const kinds = [_]u8{ Kind.core, Kind.sdhost, Kind.camera, Kind.display, Kind.ethernet, Kind.memory };
    for (kinds) |kind| {
        var chip: Module = undefined;
        const module: Module = .{ .kind = kind, .index = 0 };
        try std.testing.expectEqual(clock_profile.Err.ok, clock_profile.toChip(module, &chip));
        try std.testing.expectEqual(@as(u8, 0), chip.index);
    }
}

test "the board routes none of these, so the profile says so" {
    const kinds = [_]u8{ Kind.spi, Kind.can, Kind.adc, Kind.dac, Kind.usb, Kind.timer, Kind.pwm, Kind.dma, Kind.rtc, Kind.watchdog, Kind.crypto };
    for (kinds) |kind| {
        try std.testing.expectEqual(@as(u8, 0), clock_profile.count(kind));
    }
}

test "counts match the wiring" {
    try std.testing.expectEqual(@as(u8, 4), clock_profile.count(Kind.uart));
    try std.testing.expectEqual(@as(u8, 1), clock_profile.count(Kind.i2c));
    try std.testing.expectEqual(@as(u8, 0), clock_profile.count(200));
}

test "binding refuses a null handle and otherwise reaches fw_clock_bind" {
    bind_calls = 0;
    try std.testing.expectEqual(clock_profile.Err.invalid_arg, clock_profile.bind(null));
    try std.testing.expectEqual(@as(usize, 0), bind_calls);

    var clk: clock_profile.FwClock = .{};
    try std.testing.expectEqual(clock_profile.Err.ok, clock_profile.bind(&clk));
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
    try std.testing.expect(clk.bound);
}

test "the board handle binds once and is reused" {
    bind_calls = 0;
    const first = clock_profile.handle();
    const second = clock_profile.handle();
    try std.testing.expectEqual(first, second);
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
}

test "a board-numbered read reaches the chip binding translated" {
    chip_rate_calls = 0;
    var clk: clock_profile.FwClock = .{};
    _ = clock_profile.bind(&clk);
    var hz: u32 = 0;
    const module: Module = .{ .kind = Kind.uart, .index = 3 };
    try std.testing.expectEqual(@as(u32, 0), clk.iface.?.rate_for(null, module, &hz));
    try std.testing.expectEqual(@as(u8, 8), last_chip.index);
    try std.testing.expectEqual(@as(u32, 125_000_000), hz);
}

test "an unwired module never reaches the chip binding" {
    chip_gate_calls = 0;
    var clk: clock_profile.FwClock = .{};
    _ = clock_profile.bind(&clk);
    const module: Module = .{ .kind = Kind.spi, .index = 0 };
    try std.testing.expectEqual(clock_profile.Err.not_found, clk.iface.?.set_gate(null, module, true));
    try std.testing.expectEqual(@as(usize, 0), chip_gate_calls);
}

test "presence is answered from the board wiring, not the chip" {
    var clk: clock_profile.FwClock = .{};
    _ = clock_profile.bind(&clk);
    var present: bool = false;
    const wired: Module = .{ .kind = Kind.uart, .index = 2 };
    try std.testing.expectEqual(clock_profile.Err.ok, clk.iface.?.has_module(null, wired, &present));
    try std.testing.expect(present);

    const unwired: Module = .{ .kind = Kind.can, .index = 0 };
    try std.testing.expectEqual(clock_profile.Err.ok, clk.iface.?.has_module(null, unwired, &present));
    try std.testing.expect(!present);
}
