//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The board GPT profile against fake chip adapters and a fake pin router.
//! Held down: the channel split, that every op lands on the chip channel
//! rather than the board index, and the PWM pin route/release ordering.

const std = @import("std");
const p = @import("gpt_profile");

const ErrCode = p.ErrCode;
const TimerCh = p.TimerCh;
const PwmCh = p.PwmCh;

var last_chip: u8 = 0xFF;
var open_result: ErrCode = 0;
var close_result: ErrCode = 0;
var route_result: ErrCode = 0;
var routed_pin: u16 = 0;
var routed_psel: u8 = 0;
var released_pin: u16 = 0;
var release_calls: usize = 0;
var events: [4]u8 = .{ 0, 0, 0, 0 };
var event_len: usize = 0;

fn reset() void {
    last_chip = 0xFF;
    open_result = 0;
    close_result = 0;
    route_result = 0;
    routed_pin = 0;
    routed_psel = 0;
    released_pin = 0;
    release_calls = 0;
    event_len = 0;
}

fn note(e: u8) void {
    events[event_len] = e;
    event_len += 1;
}

const Timer = @typeInfo(@typeInfo(@FieldType(p.FwTimer, "iface")).optional.child).pointer.child;
const Pwm = @typeInfo(@typeInfo(@FieldType(p.FwPwm, "iface")).optional.child).pointer.child;
const TCaps = @typeInfo(@typeInfo(@FieldType(Timer, "get_caps")).pointer.child).@"fn".param_types[1].?;
const PCaps = @typeInfo(@typeInfo(@FieldType(Pwm, "get_caps")).pointer.child).@"fn".param_types[1].?;

fn tCaps(_: ?*anyopaque, out: TCaps) callconv(.c) ErrCode {
    out.* = .{ .channel_count = 10, .counter_bits = 32, .counter_max = 0xFFFF_FFFF };
    return 0;
}
fn tOpen(_: ?*anyopaque, ch: TimerCh, _: u8, _: u32) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}
fn tCh(_: ?*anyopaque, ch: TimerCh) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}
fn tU32(_: ?*anyopaque, ch: TimerCh, _: u32) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}
fn tOut(_: ?*anyopaque, ch: TimerCh, _: *u32) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}
fn tWrap(_: ?*anyopaque, ch: TimerCh, _: *bool) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}

const fake_timer: Timer = .{
    .get_caps = tCaps,
    .open = tOpen,
    .close = tCh,
    .start = tCh,
    .stop = tCh,
    .read = tOut,
    .set_period = tU32,
    .capture_read = tOut,
    .take_wrap = tWrap,
};

fn pCaps(_: ?*anyopaque, out: PCaps) callconv(.c) ErrCode {
    out.* = .{ .channel_count = 10, .counter_bits = 32, .period_max = 0xFFFF_FFFF };
    return 0;
}
fn pOpen(_: ?*anyopaque, ch: PwmCh, _: u32, _: u8) callconv(.c) ErrCode {
    last_chip = ch.index;
    note('o');
    return open_result;
}
fn pClose(_: ?*anyopaque, ch: PwmCh) callconv(.c) ErrCode {
    last_chip = ch.index;
    note('c');
    return close_result;
}
fn pCh(_: ?*anyopaque, ch: PwmCh) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}
fn pU32(_: ?*anyopaque, ch: PwmCh, _: u32) callconv(.c) ErrCode {
    last_chip = ch.index;
    return 0;
}

const fake_pwm: Pwm = .{
    .get_caps = pCaps,
    .open = pOpen,
    .close = pClose,
    .start = pCh,
    .stop = pCh,
    .set_period = pU32,
    .set_duty = pU32,
};

export fn fw_timer_ra8_iface() *const Timer {
    return &fake_timer;
}
export fn fw_pwm_ra8_iface() *const Pwm {
    return &fake_pwm;
}
export fn fw_timer_bind(tmr: *p.FwTimer, iface: *const Timer, ctx: ?*anyopaque) ErrCode {
    tmr.* = .{ .iface = iface, .ctx = ctx, .bound = true };
    return iface.get_caps(ctx, &tmr.caps);
}
export fn fw_pwm_bind(pwm: *p.FwPwm, iface: *const Pwm, ctx: ?*anyopaque) ErrCode {
    pwm.* = .{ .iface = iface, .ctx = ctx, .bound = true };
    return iface.get_caps(ctx, &pwm.caps);
}
export fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: [*:0]const u8) ErrCode {
    _ = owner;
    routed_pin = pin;
    routed_psel = psel;
    note('r');
    return route_result;
}
export fn ra8_pin_validator_release(pin: u16) ErrCode {
    released_pin = pin;
    release_calls += 1;
    note('x');
    return 0;
}

test "timer and pwm indices map to the board's GPT split" {
    const timers = [_]u8{ 0, 3, 4, 5, 6, 7, 9 };
    for (timers, 0..) |want, i| {
        var chip: u8 = 0xFF;
        try std.testing.expectEqual(@as(ErrCode, 0), p.timerToChip(@intCast(i), &chip));
        try std.testing.expectEqual(want, chip);
    }
    const pwms = [_]u8{ 1, 2, 8 };
    for (pwms, 0..) |want, i| {
        var chip: u8 = 0xFF;
        try std.testing.expectEqual(@as(ErrCode, 0), p.pwmToChip(@intCast(i), &chip));
        try std.testing.expectEqual(want, chip);
    }
}

test "out of range is not_found and a null out-pointer is invalid_arg" {
    var chip: u8 = 0xAA;
    try std.testing.expectEqual(@as(ErrCode, 0x106), p.timerToChip(7, &chip));
    try std.testing.expectEqual(@as(ErrCode, 0x106), p.pwmToChip(3, &chip));
    try std.testing.expectEqual(@as(u8, 0xAA), chip);
    try std.testing.expectEqual(@as(ErrCode, 0x103), p.timerToChip(0, null));
    try std.testing.expectEqual(@as(ErrCode, 0x103), p.pwmToChip(0, null));
    try std.testing.expectEqual(@as(ErrCode, 0x103), p.bindTimer(null));
    try std.testing.expectEqual(@as(ErrCode, 0x103), p.bindPwm(null));
}

test "handles bind once and report the board's channel counts" {
    const t = p.timerHandle();
    try std.testing.expect(t.bound);
    try std.testing.expectEqual(@as(u8, 7), t.caps.channel_count);
    try std.testing.expectEqual(t, p.timerHandle());
    const w = p.pwmHandle();
    try std.testing.expect(w.bound);
    try std.testing.expectEqual(@as(u8, 3), w.caps.channel_count);
}

test "timer ops land on the chip channel" {
    reset();
    const ops = p.timerHandle().iface.?;
    var counts: u32 = 0;
    try std.testing.expectEqual(@as(ErrCode, 0), ops.read(null, .{ .index = 6 }, &counts));
    try std.testing.expectEqual(@as(u8, 9), last_chip);
    try std.testing.expectEqual(@as(ErrCode, 0), ops.start(null, .{ .index = 1 }));
    try std.testing.expectEqual(@as(u8, 3), last_chip);
}

test "pwm open routes the GTIOC pin before the counter" {
    reset();
    const ops = p.pwmHandle().iface.?;
    try std.testing.expectEqual(@as(ErrCode, 0), ops.open(null, .{ .index = 1 }, 1000, 1));
    try std.testing.expectEqualSlices(u8, "ro", events[0..event_len]);
    try std.testing.expectEqual(@as(u16, 0x0103), routed_pin);
    try std.testing.expectEqual(@as(u8, 0x03), routed_psel);
    try std.testing.expectEqual(@as(u8, 2), last_chip);
}

test "a refused route never reaches the counter" {
    reset();
    route_result = 0x205;
    const ops = p.pwmHandle().iface.?;
    try std.testing.expectEqual(@as(ErrCode, 0x205), ops.open(null, .{ .index = 0 }, 1000, 1));
    try std.testing.expectEqualSlices(u8, "r", events[0..event_len]);
}

test "a refused open hands the pin back" {
    reset();
    open_result = 0x107;
    const ops = p.pwmHandle().iface.?;
    try std.testing.expectEqual(@as(ErrCode, 0x107), ops.open(null, .{ .index = 2 }, 1000, 1));
    try std.testing.expectEqualSlices(u8, "rox", events[0..event_len]);
    try std.testing.expectEqual(@as(u16, 0x0101), released_pin);
}

test "close releases the pin only after the counter closes" {
    reset();
    const ops = p.pwmHandle().iface.?;
    try std.testing.expectEqual(@as(ErrCode, 0), ops.close(null, .{ .index = 0 }));
    try std.testing.expectEqualSlices(u8, "cx", events[0..event_len]);
    try std.testing.expectEqual(@as(u16, 0x0105), released_pin);
    reset();
    close_result = 0x10F;
    try std.testing.expectEqual(@as(ErrCode, 0x10F), ops.close(null, .{ .index = 0 }));
    try std.testing.expectEqual(@as(usize, 0), release_calls);
}
