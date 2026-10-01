//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_timer` facade, driven through a fake binding. What is
//! proved here is facade behaviour no chip can change: the bind refusals, the
//! entry guards, the period and mode checks, and that reads never leave a
//! stale count behind on failure.

const std = @import("std");
const abi = @import("abi");

const Caps = abi.Caps;
const Ch = abi.Ch;
const Iface = abi.Iface;
const Mode = abi.Mode;
const Timer = abi.Timer;

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
    caps: Caps = .{
        .channel_count = 2,
        .counter_bits = 16,
        .counter_max = 0xFFFF,
        .has_capture = true,
        .has_one_shot = true,
    },
    caps_err: u16 = ok,
    op_err: u16 = ok,
    counts: u32 = 1234,
    calls: u32 = 0,
    last_mode: u8 = 0,
    last_period: u32 = 0,
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

fn fakeOpen(ctx: ?*anyopaque, _: Ch, mode: u8, period: u32) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    s.last_mode = mode;
    s.last_period = period;
    return s.op_err;
}

fn fakeCh(ctx: ?*anyopaque, _: Ch) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    return s.op_err;
}

fn fakePeriod(ctx: ?*anyopaque, _: Ch, period: u32) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    s.last_period = period;
    return s.op_err;
}

fn fakeRead(ctx: ?*anyopaque, _: Ch, out: ?*u32) callconv(.c) u16 {
    const s = state(ctx);
    s.calls += 1;
    if (s.op_err != ok) return s.op_err;
    out.?.* = s.counts;
    return ok;
}

const ops = Iface{
    .get_caps = fakeCaps,
    .open = fakeOpen,
    .close = fakeCh,
    .start = fakeCh,
    .stop = fakeCh,
    .read = fakeRead,
    .set_period = fakePeriod,
    .capture_read = fakeRead,
};

fn bound() Timer {
    fake = .{};
    var tmr = std.mem.zeroes(Timer);
    std.debug.assert(abi.fw_timer_bind(&tmr, &ops, &fake) == ok);
    return tmr;
}

test "bind snapshots the backend's caps" {
    const tmr = bound();
    try std.testing.expect(tmr.bound);
    try std.testing.expectEqual(@as(u32, 0xFFFF), tmr.caps.counter_max);
    try std.testing.expectEqual(@as(u8, 2), tmr.caps.channel_count);
}

test "bind refuses a NULL handle or ops table" {
    fake = .{};
    var tmr = std.mem.zeroes(Timer);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_bind(null, &ops, &fake));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_bind(&tmr, null, &fake));
    try std.testing.expect(!tmr.bound);
}

test "bind refuses any single unset op" {
    inline for (std.meta.fields(Iface)) |field| {
        fake = .{};
        var tmr = std.mem.zeroes(Timer);
        var partial = ops;
        @field(partial, field.name) = null;
        try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_bind(&tmr, &partial, &fake));
        try std.testing.expect(!tmr.bound);
    }
}

test "bind forwards a caps failure and stays unbound" {
    fake = .{ .caps_err = err_backend };
    var tmr = std.mem.zeroes(Timer);
    try std.testing.expectEqual(err_backend, abi.fw_timer_bind(&tmr, &ops, &fake));
    try std.testing.expect(!tmr.bound);
}

test "bind refuses a zero counter width or limit" {
    var tmr = std.mem.zeroes(Timer);
    fake = .{};
    fake.caps.counter_bits = 0;
    try std.testing.expectEqual(err_invalid_state, abi.fw_timer_bind(&tmr, &ops, &fake));
    fake = .{};
    fake.caps.counter_max = 0;
    try std.testing.expectEqual(err_invalid_state, abi.fw_timer_bind(&tmr, &ops, &fake));
    try std.testing.expect(!tmr.bound);
}

test "a zeroed handle is an error return, not a jump through null" {
    const tmr = std.mem.zeroes(Timer);
    var counts: u32 = 99;
    try std.testing.expectEqual(err_not_initialized, abi.fw_timer_start(&tmr, ch0));
    try std.testing.expectEqual(err_not_initialized, abi.fw_timer_open(&tmr, ch0, Mode.free_run, 10));
    try std.testing.expectEqual(err_not_initialized, abi.fw_timer_read(&tmr, ch0, &counts));
    try std.testing.expectEqual(@as(u32, 0), counts);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_stop(null, ch0));
}

test "get_caps zeroes its output on an unbound handle" {
    const tmr = std.mem.zeroes(Timer);
    var caps = Caps{ .channel_count = 9, .counter_bits = 9, .counter_max = 9, .has_capture = true, .has_one_shot = true };
    try std.testing.expectEqual(err_not_initialized, abi.fw_timer_get_caps(&tmr, &caps));
    try std.testing.expectEqual(@as(u32, 0), caps.counter_max);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_get_caps(&tmr, null));
}

test "a channel the board does not carry is not_found on every op" {
    const tmr = bound();
    const missing = Ch{ .index = 2 };
    var counts: u32 = 0;
    try std.testing.expectEqual(err_not_found, abi.fw_timer_open(&tmr, missing, Mode.free_run, 10));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_close(&tmr, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_start(&tmr, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_stop(&tmr, missing));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_read(&tmr, missing, &counts));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_set_period(&tmr, missing, 10));
    try std.testing.expectEqual(err_not_found, abi.fw_timer_capture_read(&tmr, missing, &counts));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a period wider than the counter is refused, not truncated" {
    const tmr = bound();
    try std.testing.expectEqual(err_out_of_range, abi.fw_timer_open(&tmr, ch0, Mode.free_run, 0x10000));
    try std.testing.expectEqual(err_out_of_range, abi.fw_timer_set_period(&tmr, ch0, 0x10000));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_open(&tmr, ch0, Mode.free_run, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
    try std.testing.expectEqual(ok, abi.fw_timer_set_period(&tmr, ch1, 0xFFFF));
    try std.testing.expectEqual(@as(u32, 0xFFFF), fake.last_period);
}

test "an unenumerated mode is an argument error" {
    const tmr = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_open(&tmr, ch0, Mode.none, 10));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_open(&tmr, ch0, Mode.count, 10));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_open(&tmr, ch0, 0xFF, 10));
}

test "a valid mode the backend declared absent is not_supported" {
    var tmr = bound();
    tmr.caps.has_capture = false;
    tmr.caps.has_one_shot = false;
    try std.testing.expectEqual(err_not_supported, abi.fw_timer_open(&tmr, ch0, Mode.capture, 10));
    try std.testing.expectEqual(err_not_supported, abi.fw_timer_open(&tmr, ch0, Mode.one_shot, 10));
    try std.testing.expectEqual(ok, abi.fw_timer_open(&tmr, ch0, Mode.free_run, 10));
    try std.testing.expectEqual(Mode.free_run, fake.last_mode);
}

test "start, stop and close forward the backend's answer" {
    const tmr = bound();
    try std.testing.expectEqual(ok, abi.fw_timer_start(&tmr, ch0));
    try std.testing.expectEqual(ok, abi.fw_timer_stop(&tmr, ch0));
    try std.testing.expectEqual(ok, abi.fw_timer_close(&tmr, ch0));
    fake.op_err = err_backend;
    try std.testing.expectEqual(err_backend, abi.fw_timer_start(&tmr, ch1));
    try std.testing.expectEqual(@as(u32, 4), fake.calls);
}

test "read passes the count through, and zeroes it on failure" {
    const tmr = bound();
    var counts: u32 = 0;
    try std.testing.expectEqual(ok, abi.fw_timer_read(&tmr, ch0, &counts));
    try std.testing.expectEqual(@as(u32, 1234), counts);
    fake.op_err = err_backend;
    try std.testing.expectEqual(err_backend, abi.fw_timer_read(&tmr, ch0, &counts));
    try std.testing.expectEqual(@as(u32, 0), counts);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_timer_read(&tmr, ch0, null));
}

test "capture_read answers declared-absent itself, before the backend" {
    var tmr = bound();
    var counts: u32 = 7;
    tmr.caps.has_capture = false;
    try std.testing.expectEqual(err_not_supported, abi.fw_timer_capture_read(&tmr, ch0, &counts));
    try std.testing.expectEqual(@as(u32, 0), counts);
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
    tmr.caps.has_capture = true;
    try std.testing.expectEqual(ok, abi.fw_timer_capture_read(&tmr, ch0, &counts));
    try std.testing.expectEqual(@as(u32, 1234), counts);
}

test "layouts match the header" {
    try std.testing.expectEqual(@as(usize, 1), @sizeOf(Ch));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(Caps));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Caps, "channel_count"));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(Caps, "counter_bits"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(Caps, "counter_max"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(Caps, "has_capture"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(Caps, "has_one_shot"));
    try std.testing.expectEqual(@as(usize, 8 * @sizeOf(usize)), @sizeOf(Iface));
    try std.testing.expectEqual(2 * @sizeOf(usize), @offsetOf(Timer, "caps"));
    try std.testing.expectEqual(2 * @sizeOf(usize) + 12, @offsetOf(Timer, "bound"));
}
