//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_pwm` facade, driven through a fake binding: the bind
//! refusals, the entry guards, and the period, polarity and duty checks no
//! chip can change.

const std = @import("std");
const abi = @import("abi");

const Caps = abi.Caps;
const Ch = abi.Ch;
const Duty = abi.Duty;
const Iface = abi.Iface;
const Polarity = abi.Polarity;
const Pwm = abi.Pwm;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_state: u16 = 0x104;
const err_not_found: u16 = 0x106;
const err_not_supported: u16 = 0x107;
const err_not_initialized: u16 = 0x10F;
const err_out_of_range: u16 = 0x208;
const err_backend: u16 = 0x201;

const ch0 = Ch{ .index = 0 };
const ch1 = Ch{ .index = 1 };

const Fake = struct {
    caps: Caps = .{ .channel_count = 2, .counter_bits = 32, .period_max = 1_000_000, .has_active_low = true },
    caps_err: u16 = ok,
    op_err: u16 = ok,
    calls: u32 = 0,
    last_pol: u8 = 0,
    last_value: u32 = 0,
};

var fake: Fake = .{};

fn state(ctx: ?*anyopaque) *Fake {
    return @ptrCast(@alignCast(ctx.?));
}

fn fakeCaps(ctx: ?*anyopaque, out: ?*Caps) callconv(.c) u16 {
    const s = state(ctx);
    if (s.caps_err != ok) return s.caps_err;
    out.?.* = s.caps;
    return ok;
}

fn fakeOpen(ctx: ?*anyopaque, _: Ch, period: u32, pol: u8) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    s.last_value = period;
    s.last_pol = pol;
    return s.op_err;
}

fn fakeCh(ctx: ?*anyopaque, _: Ch) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    return s.op_err;
}

fn fakeValue(ctx: ?*anyopaque, _: Ch, value: u32) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    s.last_value = value;
    return s.op_err;
}

const ops = Iface{
    .get_caps = fakeCaps,
    .open = fakeOpen,
    .close = fakeCh,
    .start = fakeCh,
    .stop = fakeCh,
    .set_period = fakeValue,
    .set_duty = fakeValue,
};

fn bound() Pwm {
    fake = .{};
    var pwm = std.mem.zeroes(Pwm);
    std.debug.assert(abi.fw_pwm_bind(&pwm, &ops, &fake) == ok);
    return pwm;
}

test "bind snapshots the backend's caps" {
    const pwm = bound();
    try std.testing.expect(pwm.bound);
    try std.testing.expectEqual(@as(u32, 1_000_000), pwm.caps.period_max);
}

test "bind refuses a NULL handle or ops table" {
    fake = .{};
    var pwm = std.mem.zeroes(Pwm);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_bind(null, &ops, &fake));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_bind(&pwm, null, &fake));
    try std.testing.expect(!pwm.bound);
}

test "bind refuses any single unset op" {
    inline for (std.meta.fields(Iface)) |field| {
        fake = .{};
        var pwm = std.mem.zeroes(Pwm);
        var partial = ops;
        @field(partial, field.name) = null;
        try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_bind(&pwm, &partial, &fake));
        try std.testing.expect(!pwm.bound);
    }
}

test "bind forwards a caps failure and refuses zero width or period_max" {
    var pwm = std.mem.zeroes(Pwm);
    fake = .{ .caps_err = err_backend };
    try std.testing.expectEqual(err_backend, abi.fw_pwm_bind(&pwm, &ops, &fake));
    fake = .{};
    fake.caps.counter_bits = 0;
    try std.testing.expectEqual(err_invalid_state, abi.fw_pwm_bind(&pwm, &ops, &fake));
    fake = .{};
    fake.caps.period_max = 0;
    try std.testing.expectEqual(err_invalid_state, abi.fw_pwm_bind(&pwm, &ops, &fake));
    try std.testing.expect(!pwm.bound);
}

test "a zeroed handle is an error return, not a jump through null" {
    const pwm = std.mem.zeroes(Pwm);
    try std.testing.expectEqual(err_not_initialized, abi.fw_pwm_start(&pwm, ch0));
    try std.testing.expectEqual(err_not_initialized, abi.fw_pwm_set_duty(&pwm, ch0, 0));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_stop(null, ch0));
    var caps = Caps{ .channel_count = 9, .counter_bits = 9, .period_max = 9, .has_active_low = true };
    try std.testing.expectEqual(err_not_initialized, abi.fw_pwm_get_caps(&pwm, &caps));
    try std.testing.expectEqual(@as(u32, 0), caps.period_max);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_get_caps(&pwm, null));
}

test "an output the board does not carry is not_found on every op" {
    const pwm = bound();
    const missing = Ch{ .index = 2 };
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_open(&pwm, missing, 10, Polarity.active_high));
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_close(&pwm, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_start(&pwm, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_stop(&pwm, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_set_period(&pwm, missing, 10));
    try std.testing.expectEqual(err_not_found, abi.fw_pwm_set_duty(&pwm, missing, 10));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a period of zero or above period_max is refused" {
    const pwm = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_open(&pwm, ch0, 0, Polarity.active_high));
    try std.testing.expectEqual(err_out_of_range, abi.fw_pwm_open(&pwm, ch0, 1_000_001, Polarity.active_high));
    try std.testing.expectEqual(err_out_of_range, abi.fw_pwm_set_period(&pwm, ch0, 1_000_001));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
    try std.testing.expectEqual(ok, abi.fw_pwm_set_period(&pwm, ch1, 1_000_000));
    try std.testing.expectEqual(@as(u32, 1_000_000), fake.last_value);
}

test "polarity: unenumerated is invalid_arg, absent active-low is not_supported" {
    var pwm = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_open(&pwm, ch0, 10, Polarity.none));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_pwm_open(&pwm, ch0, 10, Polarity.count));
    pwm.caps.has_active_low = false;
    try std.testing.expectEqual(err_not_supported, abi.fw_pwm_open(&pwm, ch0, 10, Polarity.active_low));
    try std.testing.expectEqual(ok, abi.fw_pwm_open(&pwm, ch0, 10, Polarity.active_high));
    try std.testing.expectEqual(Polarity.active_high, fake.last_pol);
}

test "duty is inclusive of full scale and refused above it" {
    const pwm = bound();
    try std.testing.expectEqual(ok, abi.fw_pwm_set_duty(&pwm, ch0, Duty.full));
    try std.testing.expectEqual(Duty.full, fake.last_value);
    try std.testing.expectEqual(ok, abi.fw_pwm_set_duty(&pwm, ch0, 0));
    try std.testing.expectEqual(err_out_of_range, abi.fw_pwm_set_duty(&pwm, ch0, Duty.full + 1));
    try std.testing.expectEqual(@as(u32, 2), fake.calls);
}

test "backend errors pass through unchanged" {
    const pwm = bound();
    try std.testing.expectEqual(ok, abi.fw_pwm_start(&pwm, ch0));
    try std.testing.expectEqual(ok, abi.fw_pwm_stop(&pwm, ch0));
    try std.testing.expectEqual(ok, abi.fw_pwm_close(&pwm, ch0));
    fake.op_err = err_backend;
    try std.testing.expectEqual(err_backend, abi.fw_pwm_start(&pwm, ch1));
    try std.testing.expectEqual(err_backend, abi.fw_pwm_open(&pwm, ch1, 10, Polarity.active_high));
    try std.testing.expectEqual(err_backend, abi.fw_pwm_set_duty(&pwm, ch1, 100));
}

test "layouts match the header" {
    try std.testing.expectEqual(@as(usize, 1), @sizeOf(Ch));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(Caps));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(Caps, "counter_bits"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(Caps, "period_max"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(Caps, "has_active_low"));
    try std.testing.expectEqual(@as(usize, 7 * @sizeOf(usize)), @sizeOf(Iface));
    try std.testing.expectEqual(2 * @sizeOf(usize), @offsetOf(Pwm, "caps"));
    try std.testing.expectEqual(2 * @sizeOf(usize) + 12, @offsetOf(Pwm, "bound"));
}
