//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_timer.h`: the counter half of the timer/PWM
//! split, exported to C. Same story as `fw_if_clock_abi.zig`: a backend hands
//! over an ops table at bind time, the facade snapshots its capabilities
//! then, and every entry point is a guard and a forward.
//!
//! The checks are the value. A backend handed a period wider than its
//! counter does not fail, it truncates, and the interval comes out wrong by a
//! factor nobody notices until something downstream is out of spec. Refusing
//! at the seam turns that into an out-of-range error at the caller's line.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_timer_mode_t`, `uint8_t`-backed as the header declares it.
pub const Mode = struct {
    pub const none: u8 = 0;
    pub const free_run: u8 = 1;
    pub const one_shot: u8 = 2;
    pub const capture: u8 = 3;
    pub const count: u8 = 4;
};

/// `fw_timer_ch_t`: a zero-based board instance.
pub const Ch = extern struct {
    index: u8,
};

/// `fw_timer_caps_t`: what a backend reported at bind time.
pub const Caps = extern struct {
    channel_count: u8,
    counter_bits: u8,
    counter_max: u32,
    has_capture: bool,
    has_one_shot: bool,
};

/// `fw_timer_iface_t`: the ops a chip-and-board binding fills.
pub const Iface = extern struct {
    get_caps: ?*const fn (?*anyopaque, ?*Caps) callconv(.c) Err,
    open: ?*const fn (?*anyopaque, Ch, u8, u32) callconv(.c) Err,
    close: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    start: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    stop: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    read: ?*const fn (?*anyopaque, Ch, ?*u32) callconv(.c) Err,
    set_period: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) Err,
    capture_read: ?*const fn (?*anyopaque, Ch, ?*u32) callconv(.c) Err,
    /// Whether the count reached its period since the last call; clears it.
    take_wrap: ?*const fn (?*anyopaque, Ch, ?*bool) callconv(.c) Err,
};

/// `fw_timer_t`: the caller-owned binding handle.
pub const Timer = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    caps: Caps,
    bound: bool,
};

const zero_caps = std.mem.zeroes(Caps);

/// A NULL op is a malformed binding, not a declined capability. Every op is
/// checked by name off the struct, so a tenth op cannot be forgotten here.
fn complete(ops: *const Iface) bool {
    inline for (std.meta.fields(Iface)) |field| {
        if (@field(ops, field.name) == null) return false;
    }
    return true;
}

/// Entry guard: non-NULL, bound, and a channel the board carries.
fn check(tmr: ?*const Timer, ch: Ch) Err {
    const handle = tmr orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    if (ch.index >= handle.caps.channel_count) return core.err_not_found;
    return core.ok;
}

fn periodOk(caps: Caps, period: u32) Err {
    if (period == 0) return core.err_invalid_arg;
    if (period > caps.counter_max) return core.err_out_of_range;
    return core.ok;
}

/// An unenumerated mode is the caller's mistake; a valid one this backend
/// declared absent is the backend's answer.
fn modeOk(caps: Caps, mode: u8) Err {
    if (mode == Mode.none or mode >= Mode.count) return core.err_invalid_arg;
    if (mode == Mode.capture and !caps.has_capture) return core.err_not_supported;
    if (mode == Mode.one_shot and !caps.has_one_shot) return core.err_not_supported;
    return core.ok;
}

pub export fn fw_timer_bind(tmr: ?*Timer, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = tmr orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    if (!complete(ops)) return core.err_invalid_arg;

    var caps = zero_caps;
    const err = ops.get_caps.?(ctx, &caps);
    if (err != core.ok) return err;
    // A zero width or limit makes every later period check vacuous.
    if (caps.counter_bits == 0 or caps.counter_max == 0) return core.err_invalid_state;

    handle.* = .{ .iface = ops, .ctx = ctx, .caps = caps, .bound = true };
    return core.ok;
}

pub export fn fw_timer_get_caps(tmr: ?*const Timer, out: ?*Caps) callconv(.c) Err {
    const dst = out orelse return core.err_invalid_arg;
    dst.* = zero_caps;
    const handle = tmr orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    dst.* = handle.caps;
    return core.ok;
}

pub export fn fw_timer_open(tmr: ?*const Timer, ch: Ch, mode: u8, period: u32) callconv(.c) Err {
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    const handle = tmr.?;
    const mode_err = modeOk(handle.caps, mode);
    if (mode_err != core.ok) return mode_err;
    const period_err = periodOk(handle.caps, period);
    if (period_err != core.ok) return period_err;
    return handle.iface.?.open.?(handle.ctx, ch, mode, period);
}

pub export fn fw_timer_close(tmr: ?*const Timer, ch: Ch) callconv(.c) Err {
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    return tmr.?.iface.?.close.?(tmr.?.ctx, ch);
}

pub export fn fw_timer_start(tmr: ?*const Timer, ch: Ch) callconv(.c) Err {
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    return tmr.?.iface.?.start.?(tmr.?.ctx, ch);
}

pub export fn fw_timer_stop(tmr: ?*const Timer, ch: Ch) callconv(.c) Err {
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    return tmr.?.iface.?.stop.?(tmr.?.ctx, ch);
}

pub export fn fw_timer_set_period(tmr: ?*const Timer, ch: Ch, period: u32) callconv(.c) Err {
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    const period_err = periodOk(tmr.?.caps, period);
    if (period_err != core.ok) return period_err;
    return tmr.?.iface.?.set_period.?(tmr.?.ctx, ch, period);
}

/// Both reads zero their output first, so a caller that ignores the status
/// cannot take a stale count for a fresh one.
pub export fn fw_timer_read(tmr: ?*const Timer, ch: Ch, out_counts: ?*u32) callconv(.c) Err {
    const out = out_counts orelse return core.err_invalid_arg;
    out.* = 0;
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    return forwardCount(tmr.?.iface.?.read.?, tmr.?.ctx, ch, out);
}

pub export fn fw_timer_capture_read(tmr: ?*const Timer, ch: Ch, out_counts: ?*u32) callconv(.c) Err {
    const out = out_counts orelse return core.err_invalid_arg;
    out.* = 0;
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    // Declared absent is answered here, so "this backend cannot" never looks
    // like "no edge has arrived yet".
    if (!tmr.?.caps.has_capture) return core.err_not_supported;
    return forwardCount(tmr.?.iface.?.capture_read.?, tmr.?.ctx, ch, out);
}

/// Sticky until taken, read-and-clear in one call. Writes false first and on
/// every failure, so no stale true leaks to a caller ignoring the status.
pub export fn fw_timer_take_wrap(tmr: ?*const Timer, ch: Ch, out_wrapped: ?*bool) callconv(.c) Err {
    const out = out_wrapped orelse return core.err_invalid_arg;
    out.* = false;
    const guard = check(tmr, ch);
    if (guard != core.ok) return guard;
    var wrapped = false;
    const err = tmr.?.iface.?.take_wrap.?(tmr.?.ctx, ch, &wrapped);
    if (err != core.ok) return err;
    out.* = wrapped;
    return core.ok;
}

fn forwardCount(
    op: *const fn (?*anyopaque, Ch, ?*u32) callconv(.c) Err,
    ctx: ?*anyopaque,
    ch: Ch,
    out: *u32,
) Err {
    var counts: u32 = 0;
    const err = op(ctx, ch, &counts);
    if (err != core.ok) return err;
    out.* = counts;
    return core.ok;
}
