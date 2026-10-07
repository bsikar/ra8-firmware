//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_pwm.h`: the pin-driving half of the timer/PWM
//! split, exported to C. Same shape as `fw_if_timer_abi.zig`: caps are
//! snapshotted at bind, and every entry point is a guard and a forward.
//!
//! Duty is a Q16 ratio of the period, not a compare count, so retuning the
//! period keeps the duty; the binding owns the scaling to counts. Polarity
//! is fixed at open, since flipping it mid-run is a glitch on the pin.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_pwm_duty_t` scale: 65536 is exactly 100%, so the range is inclusive.
pub const Duty = struct {
    pub const full: u32 = 65536;
};

/// `fw_pwm_polarity_t`, `uint8_t`-backed as the header declares it.
pub const Polarity = struct {
    pub const none: u8 = 0;
    pub const active_high: u8 = 1;
    pub const active_low: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_pwm_ch_t`: a zero-based board output.
pub const Ch = extern struct {
    index: u8,
};

/// `fw_pwm_caps_t`: what a backend reported at bind time.
pub const Caps = extern struct {
    channel_count: u8,
    counter_bits: u8,
    period_max: u32,
    has_active_low: bool,
};

/// `fw_pwm_iface_t`: the ops a chip-and-board binding fills.
pub const Iface = extern struct {
    get_caps: ?*const fn (?*anyopaque, ?*Caps) callconv(.c) Err,
    open: ?*const fn (?*anyopaque, Ch, u32, u8) callconv(.c) Err,
    close: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    start: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    stop: ?*const fn (?*anyopaque, Ch) callconv(.c) Err,
    set_period: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) Err,
    set_duty: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) Err,
};

/// `fw_pwm_t`: the caller-owned binding handle.
pub const Pwm = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    caps: Caps,
    bound: bool,
};

const zero_caps = std.mem.zeroes(Caps);

/// A NULL op is a malformed binding; checked off the struct's own fields.
fn complete(ops: *const Iface) bool {
    inline for (@typeInfo(Iface).@"struct".field_names) |name| {
        if (@field(ops, name) == null) return false;
    }
    return true;
}

/// Entry guard: non-NULL, bound, and an output the board carries.
fn check(pwm: ?*const Pwm, ch: Ch) Err {
    const handle = pwm orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    if (ch.index >= handle.caps.channel_count) return core.err_not_found;
    return core.ok;
}

fn periodOk(caps: Caps, period: u32) Err {
    if (period == 0) return core.err_invalid_arg;
    if (period > caps.period_max) return core.err_out_of_range;
    return core.ok;
}

fn polarityOk(caps: Caps, pol: u8) Err {
    if (pol == Polarity.none or pol >= Polarity.count) return core.err_invalid_arg;
    if (pol == Polarity.active_low and !caps.has_active_low) return core.err_not_supported;
    return core.ok;
}

pub export fn fw_pwm_bind(pwm: ?*Pwm, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = pwm orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    if (!complete(ops)) return core.err_invalid_arg;

    var caps = zero_caps;
    const err = ops.get_caps.?(ctx, &caps);
    if (err != core.ok) return err;
    // A zero width or period_max makes every later period check vacuous.
    if (caps.counter_bits == 0 or caps.period_max == 0) return core.err_invalid_state;

    handle.* = .{ .iface = ops, .ctx = ctx, .caps = caps, .bound = true };
    return core.ok;
}

pub export fn fw_pwm_get_caps(pwm: ?*const Pwm, out: ?*Caps) callconv(.c) Err {
    const dst = out orelse return core.err_invalid_arg;
    dst.* = zero_caps;
    const handle = pwm orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    dst.* = handle.caps;
    return core.ok;
}

pub export fn fw_pwm_open(pwm: ?*const Pwm, ch: Ch, period: u32, pol: u8) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    const handle = pwm.?;
    const pol_err = polarityOk(handle.caps, pol);
    if (pol_err != core.ok) return pol_err;
    const period_err = periodOk(handle.caps, period);
    if (period_err != core.ok) return period_err;
    return handle.iface.?.open.?(handle.ctx, ch, period, pol);
}

pub export fn fw_pwm_close(pwm: ?*const Pwm, ch: Ch) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    return pwm.?.iface.?.close.?(pwm.?.ctx, ch);
}

pub export fn fw_pwm_start(pwm: ?*const Pwm, ch: Ch) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    return pwm.?.iface.?.start.?(pwm.?.ctx, ch);
}

pub export fn fw_pwm_stop(pwm: ?*const Pwm, ch: Ch) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    return pwm.?.iface.?.stop.?(pwm.?.ctx, ch);
}

pub export fn fw_pwm_set_period(pwm: ?*const Pwm, ch: Ch, period: u32) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    const period_err = periodOk(pwm.?.caps, period);
    if (period_err != core.ok) return period_err;
    return pwm.?.iface.?.set_period.?(pwm.?.ctx, ch, period);
}

pub export fn fw_pwm_set_duty(pwm: ?*const Pwm, ch: Ch, duty: u32) callconv(.c) Err {
    const guard = check(pwm, ch);
    if (guard != core.ok) return guard;
    // Above full scale has no waveform; a backend scaling it would wrap.
    if (duty > Duty.full) return core.err_out_of_range;
    return pwm.?.iface.?.set_duty.?(pwm.?.ctx, ch, duty);
}
