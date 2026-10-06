//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `fw_if_pwm_ra8.h`: the RA8 GPT32 binding of the neutral PWM port, on the
//! GTIOCnA output of each channel.
//!
//! The port's duty is a Q16 ratio of the period; GTCCR wants counts, so the
//! adapter keeps each output's period, duty and running flag and recomputes
//! the compare whenever either changes. While running the compare goes to the
//! buffer (GTCCRC) so it lands at the next wrap without a glitch; while
//! stopped it is also written straight into GTCCRA, since no wrap will come
//! to transfer it.

const Err = @import("err").Err;
const claim = @import("claim");
const hal = @import("gpt_hal");

/// `fw_pwm_ra8_limits_t`. `period + 1` must fit 32 bits.
pub const Limits = struct {
    pub const channel_count: u8 = claim.channel_count;
    pub const counter_bits: u8 = 32;
    pub const period_max: u32 = 0xFFFF_FFFE;
};

/// `K_FW_PWM_DUTY_FULL`: 65536 is exactly 100%.
pub const duty_full: u64 = 65536;

/// `fw_pwm_polarity_t`: only active-low changes the pin setup.
const active_low: u8 = 2;

/// `fw_pwm_ch_t`.
pub const Ch = extern struct {
    index: u8,
};

/// `fw_pwm_caps_t`.
pub const Caps = extern struct {
    channel_count: u8,
    counter_bits: u8,
    period_max: u32,
    has_active_low: bool,
};

/// `fw_pwm_iface_t`.
pub const Iface = extern struct {
    get_caps: ?*const fn (?*anyopaque, ?*Caps) callconv(.c) u16,
    open: ?*const fn (?*anyopaque, Ch, u32, u8) callconv(.c) u16,
    close: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    start: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    stop: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    set_period: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) u16,
    set_duty: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) u16,
};

/// `fw_pwm_t`, opaque here: only its address crosses to `fw_pwm_bind`.
pub const Pwm = opaque {};

extern fn fw_pwm_bind(pwm: ?*Pwm, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u16;

/// One output's state between calls.
const Out = struct {
    period: u32 = 0,
    duty: u32 = 0,
    running: bool = false,
};

var outs: [Limits.channel_count]Out = @splat(.{});

fn isOpen(ch: Ch) bool {
    return claim.ownedBy(ch.index, .pwm);
}

/// Compare counts for a Q16 duty: `(period + 1) * duty / 65536`.
pub fn compareFor(period: u32, duty: u32) u32 {
    const counts: u64 = @as(u64, period) + 1;
    return @truncate((counts * duty) / duty_full);
}

fn applyDuty(ch: Ch) u16 {
    const out = outs[ch.index];
    const compare = compareFor(out.period, out.duty);
    const err = hal.ra8_gpt_duty_cycle_set(ch.index, hal.Pin.a, compare);
    if (err != Err.ok or out.running) return err;
    return hal.ra8_gpt_set_duty(ch.index, hal.Ccr.a, compare);
}

fn configurePin(ch: Ch, polarity: u8) u16 {
    const low = polarity == active_low;
    const pin: hal.PinCfg = .{
        .output_enable = true,
        .polarity = if (low) hal.Polarity.active_low else hal.Polarity.active_high,
        .stop_level = if (low) hal.StopLevel.high else hal.StopLevel.low,
        .disable_on_fault = hal.Disable.none,
    };
    return hal.ra8_gpt_pwm_pin_configure(ch.index, hal.Pin.a, &pin);
}

fn getCaps(_: ?*anyopaque, out: ?*Caps) callconv(.c) u16 {
    const caps = out orelse return Err.invalid_arg;
    caps.* = .{
        .channel_count = Limits.channel_count,
        .counter_bits = Limits.counter_bits,
        .period_max = Limits.period_max,
        .has_active_low = true,
    };
    return Err.ok;
}

fn open(_: ?*anyopaque, ch: Ch, period: u32, polarity: u8) callconv(.c) u16 {
    const claimed = claim.claim(ch.index, .pwm);
    if (claimed != Err.ok) return claimed;

    outs[ch.index] = .{ .period = period };
    const cfg = hal.sawCfg(hal.Mode.saw_pwm, period);
    var err = hal.ra8_gpt_init(ch.index, &cfg);
    if (err == Err.ok) err = configurePin(ch, polarity);
    if (err == Err.ok) err = applyDuty(ch);
    if (err != Err.ok) {
        _ = hal.ra8_gpt_deinit(ch.index);
        claim.release(ch.index, .pwm);
    }
    return err;
}

fn close(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    claim.release(ch.index, .pwm);
    outs[ch.index].running = false;
    return hal.ra8_gpt_deinit(ch.index);
}

fn start(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    const err = hal.ra8_gpt_start(ch.index);
    if (err == Err.ok) outs[ch.index].running = true;
    return err;
}

fn stop(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    const err = hal.ra8_gpt_stop(ch.index);
    if (err == Err.ok) outs[ch.index].running = false;
    return err;
}

fn setPeriod(_: ?*anyopaque, ch: Ch, period: u32) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    const err = hal.ra8_gpt_period_set(ch.index, period);
    if (err != Err.ok) return err;
    outs[ch.index].period = period;
    return applyDuty(ch);
}

fn setDuty(_: ?*anyopaque, ch: Ch, duty: u32) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    outs[ch.index].duty = duty;
    return applyDuty(ch);
}

/// Static, context-free: the GPT block is a chip singleton.
const iface: Iface = .{
    .get_caps = &getCaps,
    .open = &open,
    .close = &close,
    .start = &start,
    .stop = &stop,
    .set_period = &setPeriod,
    .set_duty = &setDuty,
};

pub export fn fw_pwm_ra8_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_pwm_ra8_bind(pwm: ?*Pwm) callconv(.c) u16 {
    return fw_pwm_bind(pwm, &iface, null);
}
