//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `fw_if_timer_ra8.h`: the RA8 GPT32 binding of the neutral timer port.
//!
//! Every op but `get_caps` refuses a channel this port has not claimed, so a
//! channel the PWM adapter holds reads `invalid_state` here. Capture is not
//! offered through `open` (`has_capture = false`): a board binding with real
//! edge routes opens it with `fw_timer_ra8_open_capture`, and `capture_read`
//! then reads GTCCRA (see `timer_capture.zig`). `take_wrap` reports and clears GTST.TCFPO,
//! which sets when the count reaches GTPR in free-run and at the end of a
//! one-shot alike, and stays set until written clear.

const Err = @import("err").Err;
const claim = @import("claim");
const hal = @import("gpt_hal");
const capture = @import("timer_capture.zig");

/// `fw_timer_ra8_limits_t`.
pub const Limits = struct {
    pub const channel_count: u8 = claim.channel_count;
    pub const counter_bits: u8 = 32;
    pub const counter_max: u32 = 0xFFFF_FFFF;
};

/// `fw_timer_mode_t`: the two modes this binding accepts.
const Mode = struct {
    const free_run: u8 = 1;
    const one_shot: u8 = 2;
};

/// `fw_timer_ch_t`.
pub const Ch = extern struct {
    index: u8,
};

/// `fw_timer_caps_t`.
pub const Caps = extern struct {
    channel_count: u8,
    counter_bits: u8,
    counter_max: u32,
    has_capture: bool,
    has_one_shot: bool,
};

/// `fw_timer_iface_t`.
pub const Iface = extern struct {
    get_caps: ?*const fn (?*anyopaque, ?*Caps) callconv(.c) u16,
    open: ?*const fn (?*anyopaque, Ch, u8, u32) callconv(.c) u16,
    close: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    start: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    stop: ?*const fn (?*anyopaque, Ch) callconv(.c) u16,
    read: ?*const fn (?*anyopaque, Ch, ?*u32) callconv(.c) u16,
    set_period: ?*const fn (?*anyopaque, Ch, u32) callconv(.c) u16,
    capture_read: ?*const fn (?*anyopaque, Ch, ?*u32) callconv(.c) u16,
    take_wrap: ?*const fn (?*anyopaque, Ch, ?*bool) callconv(.c) u16,
};

/// `fw_timer_t`, opaque here: only its address crosses to `fw_timer_bind`.
pub const Timer = opaque {};

extern fn fw_timer_bind(tmr: ?*Timer, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u16;

fn isOpen(ch: Ch) bool {
    return claim.ownedBy(ch.index, .timer);
}

fn getCaps(_: ?*anyopaque, out: ?*Caps) callconv(.c) u16 {
    const caps = out orelse return Err.invalid_arg;
    caps.* = .{
        .channel_count = Limits.channel_count,
        .counter_bits = Limits.counter_bits,
        .counter_max = Limits.counter_max,
        .has_capture = false,
        .has_one_shot = true,
    };
    return Err.ok;
}

fn open(_: ?*anyopaque, ch: Ch, mode: u8, period: u32) callconv(.c) u16 {
    const gpt_mode = switch (mode) {
        Mode.free_run => hal.Mode.saw_pwm,
        Mode.one_shot => hal.Mode.saw_one_shot,
        else => return Err.not_supported,
    };
    const claimed = claim.claim(ch.index, .timer);
    if (claimed != Err.ok) return claimed;

    const cfg = hal.sawCfg(gpt_mode, period);
    const err = hal.ra8_gpt_init(ch.index, &cfg);
    if (err != Err.ok) claim.release(ch.index, .timer);
    return err;
}

fn close(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    capture.disarm(ch.index);
    claim.release(ch.index, .timer);
    return hal.ra8_gpt_deinit(ch.index);
}

fn start(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    return hal.ra8_gpt_start(ch.index);
}

fn stop(_: ?*anyopaque, ch: Ch) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    return hal.ra8_gpt_stop(ch.index);
}

fn read(_: ?*anyopaque, ch: Ch, out_counts: ?*u32) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    return hal.ra8_gpt_read(ch.index, out_counts);
}

fn setPeriod(_: ?*anyopaque, ch: Ch, period: u32) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    return hal.ra8_gpt_period_set(ch.index, period);
}

fn captureRead(_: ?*anyopaque, ch: Ch, out_counts: ?*u32) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    return capture.read(ch.index, out_counts);
}

fn takeWrap(_: ?*anyopaque, ch: Ch, out_wrapped: ?*bool) callconv(.c) u16 {
    if (!isOpen(ch)) return Err.invalid_state;
    const wrapped = out_wrapped orelse return Err.invalid_arg;

    var status: u32 = 0;
    const err = hal.ra8_gpt_get_status(ch.index, &status);
    if (err != Err.ok) return err;

    wrapped.* = (status & hal.Status.overflow) != 0;
    if (!wrapped.*) return Err.ok;
    return hal.ra8_gpt_clear_status(ch.index, hal.Status.overflow);
}

/// Static, context-free: the GPT block is a chip singleton.
const iface: Iface = .{
    .get_caps = &getCaps,
    .open = &open,
    .close = &close,
    .start = &start,
    .stop = &stop,
    .read = &read,
    .set_period = &setPeriod,
    .capture_read = &captureRead,
    .take_wrap = &takeWrap,
};

pub export fn fw_timer_ra8_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_timer_ra8_bind(tmr: ?*Timer) callconv(.c) u16 {
    return fw_timer_bind(tmr, &iface, null);
}

/// Open `ch` free-running with GTCCRA capturing on `source_mask`
/// (`ra8_gpt_capture_src_t` bits). Undone by the ordinary `close`.
pub export fn fw_timer_ra8_open_capture(ch: Ch, period: u32, source_mask: u32) callconv(.c) u16 {
    if (ch.index >= Limits.channel_count) return Err.not_found;
    if (period == 0 or !capture.validSources(source_mask)) return Err.invalid_arg;

    const opened = open(null, ch, Mode.free_run, period);
    if (opened != Err.ok) return opened;

    const err = capture.arm(ch.index, source_mask);
    if (err != Err.ok) _ = close(null, ch);
    return err;
}
