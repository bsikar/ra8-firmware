//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_clock` facade, driven through a fake binding that
//! answers from a small table. Everything proved here is facade behaviour no
//! chip can change: the entry guards, the malformed-binding rejection, the
//! zero-rate refusal and the comparison in `fw_clock_require`.

const std = @import("std");
const abi = @import("abi");

const Clock = abi.Clock;
const Iface = abi.Iface;
const Module = abi.Module;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_state: u16 = 0x104;
const err_not_supported: u16 = 0x107;
const err_not_initialized: u16 = 0x10F;
const err_busy: u16 = 0x109;

const uart0 = Module{ .kind = 2, .index = 0 };

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    rate_hz: u32 = 48_000_000,
    rate_err: u16 = ok,
    gate_err: u16 = ok,
    present: bool = true,
    last_on: bool = false,
    last_module: Module = .{ .kind = 0, .index = 0 },
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeRateFor(ctx: ?*anyopaque, module: Module, out_hz: ?*u32) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_module = module;
    if (state.rate_err != ok) return state.rate_err;
    out_hz.?.* = state.rate_hz;
    return ok;
}

fn fakeSetGate(ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_module = module;
    state.last_on = on;
    return state.gate_err;
}

fn fakeHasModule(ctx: ?*anyopaque, module: Module, out_present: ?*bool) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_module = module;
    out_present.?.* = state.present;
    return ok;
}

const fake_ops = Iface{
    .rate_for = fakeRateFor,
    .set_gate = fakeSetGate,
    .has_module = fakeHasModule,
};

fn bound() Clock {
    fake = .{};
    var clk = std.mem.zeroes(Clock);
    std.debug.assert(abi.fw_clock_bind(&clk, &fake_ops, &fake) == ok);
    return clk;
}

test "bind rejects a null handle or a null ops struct" {
    var clk = std.mem.zeroes(Clock);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_bind(&clk, null, null));
    try std.testing.expect(!clk.bound);
}

test "bind rejects a malformed binding: a NULL op is not a declined capability" {
    var clk = std.mem.zeroes(Clock);
    inline for (.{ "rate_for", "set_gate", "has_module" }) |field| {
        var ops = fake_ops;
        @field(ops, field) = null;
        try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_bind(&clk, &ops, null));
        try std.testing.expect(!clk.bound);
    }
}

test "an unbound handle is not-initialized, not a jump through a null pointer" {
    var clk = std.mem.zeroes(Clock);
    var hz: u32 = 7;
    try std.testing.expectEqual(err_not_initialized, abi.fw_clock_rate_for(&clk, uart0, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);
    try std.testing.expectEqual(err_not_initialized, abi.fw_clock_enable(&clk, uart0));
    try std.testing.expectEqual(err_not_initialized, abi.fw_clock_disable(&clk, uart0));
}

test "a zeroed module kind is rejected, and so is one past the last enumerator" {
    var clk = bound();
    var hz: u32 = 0;
    const none = Module{ .kind = 0, .index = 0 };
    const past = Module{ .kind = 20, .index = 0 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_rate_for(&clk, none, &hz));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_rate_for(&clk, past, &hz));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "rate_for reports the binding's rate and zeroes the out-parameter first" {
    var clk = bound();
    var hz: u32 = 123;
    try std.testing.expectEqual(ok, abi.fw_clock_rate_for(&clk, uart0, &hz));
    try std.testing.expectEqual(@as(u32, 48_000_000), hz);
    try std.testing.expectEqual(@as(u8, 2), fake.last_module.kind);

    fake.rate_err = err_busy;
    hz = 123;
    try std.testing.expectEqual(err_busy, abi.fw_clock_rate_for(&clk, uart0, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);
}

test "a zero rate from a binding is a bug at the seam, not a division later" {
    var clk = bound();
    fake.rate_hz = 0;
    var hz: u32 = 9;
    try std.testing.expectEqual(err_invalid_state, abi.fw_clock_rate_for(&clk, uart0, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);
}

test "rate_for rejects a null out-parameter before it touches the handle" {
    var clk = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_rate_for(&clk, uart0, null));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "require compares, and a floor of zero asks nothing" {
    var clk = bound();
    var hz: u32 = 5;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_require(&clk, uart0, 0, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);

    try std.testing.expectEqual(ok, abi.fw_clock_require(&clk, uart0, 1_000_000, &hz));
    try std.testing.expectEqual(@as(u32, 48_000_000), hz);
}

test "require leaves the rate written when the floor is not met" {
    var clk = bound();
    var hz: u32 = 0;
    const err = abi.fw_clock_require(&clk, uart0, 96_000_000, &hz);
    try std.testing.expectEqual(err_not_supported, err);
    try std.testing.expectEqual(@as(u32, 48_000_000), hz);
}

test "enable and disable carry the on flag through to the binding" {
    var clk = bound();
    try std.testing.expectEqual(ok, abi.fw_clock_enable(&clk, uart0));
    try std.testing.expect(fake.last_on);
    try std.testing.expectEqual(ok, abi.fw_clock_disable(&clk, uart0));
    try std.testing.expect(!fake.last_on);

    fake.gate_err = err_not_supported;
    try std.testing.expectEqual(err_not_supported, abi.fw_clock_enable(&clk, uart0));
}

test "has_module reports the binding's answer and clears out_present first" {
    var clk = bound();
    var present = true;
    fake.present = false;
    try std.testing.expectEqual(ok, abi.fw_clock_has_module(&clk, uart0, &present));
    try std.testing.expect(!present);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_clock_has_module(&clk, uart0, null));
}
