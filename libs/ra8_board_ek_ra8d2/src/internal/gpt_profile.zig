//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The board's split of its GPT channels between the timer and PWM ports, and
//! the `fw_if_timer` / `fw_if_pwm` bindings over the chip GPT adapters.
//!
//! Three channels have their GTIOCnA on an Arduino header pin and become PWM
//! outputs: GPT1 on D6, GPT2 on D10, GPT8 on D11. The other seven of GPT0..9
//! are timers. Board index is the subscript, chip channel is the value.

const gpt = @import("gpt_types.zig");
const seam = @import("gpt_hal.zig");
const vocab = @import("vocab.zig");

pub const ErrCode = gpt.ErrCode;
pub const TimerCh = gpt.TimerCh;
pub const PwmCh = gpt.PwmCh;
pub const FwTimer = gpt.FwTimer;
pub const FwPwm = gpt.FwPwm;

/// `k_ra8_psel_gpt0` in ra8_gpio_constants.h: 00011b, GTIOCnA/B timer I/O.
/// Not `vocab.Psel.gpt0`, which is 0x02, the GPT trigger-pin code.
const psel_gtioc: u8 = 0x03;

const ok: ErrCode = @intCast(vocab.Err.ok);
const invalid_arg: ErrCode = @intCast(vocab.Err.invalid_arg);
const not_found: ErrCode = @intCast(vocab.Err.not_found);

/// One PWM output: its GPT channel, the pin carrying GTIOCnA, and the
/// pin-validator owner name. Pins are `RA8_PIN(port, pin)`, port in the high
/// byte, from ra8_board_ek_ra8d2_connectors.h.
const PwmRow = struct { chip: u8, pin: u16, owner: [*:0]const u8 };

pub const pwm_rows = [_]PwmRow{
    .{ .chip = 1, .pin = 0x0105, .owner = "board.pwm.d6" }, // P105
    .{ .chip = 2, .pin = 0x0103, .owner = "board.pwm.d10" }, // P103
    .{ .chip = 8, .pin = 0x0101, .owner = "board.pwm.d11" }, // P101
};

pub const timer_chips = [_]u8{ 0, 3, 4, 5, 6, 7, 9 };

pub const pwm_count: u8 = pwm_rows.len;
pub const timer_count: u8 = timer_chips.len;

/// Board timer @p index -> GPT channel.
pub fn timerToChip(index: u8, out_chip: ?*u8) ErrCode {
    const dst = out_chip orelse return invalid_arg;
    if (index >= timer_count) return not_found;
    dst.* = timer_chips[index];
    return ok;
}

/// Board PWM @p index -> GPT channel.
pub fn pwmToChip(index: u8, out_chip: ?*u8) ErrCode {
    const dst = out_chip orelse return invalid_arg;
    if (index >= pwm_count) return not_found;
    dst.* = pwm_rows[index].chip;
    return ok;
}

// ------------------------------------------------------------------ timer --

/// The facade checks the index against `channel_count` before an op reaches
/// here, so an out-of-range lookup cannot happen; it maps to GPT0 regardless,
/// matching the C it replaces.
fn timerChip(ch: TimerCh) TimerCh {
    var chip: u8 = 0;
    _ = timerToChip(ch.index, &chip);
    return .{ .index = chip };
}

fn chipTimer() *const gpt.TimerIface {
    return seam.fw_timer_ra8_iface();
}

fn timerCaps(ctx: ?*anyopaque, out: *gpt.TimerCaps) callconv(.c) ErrCode {
    _ = ctx;
    const err = chipTimer().get_caps(null, out);
    if (err == ok) out.channel_count = timer_count;
    return err;
}

fn timerOpen(ctx: ?*anyopaque, ch: TimerCh, mode: u8, period: u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().open(null, timerChip(ch), mode, period);
}

fn timerClose(ctx: ?*anyopaque, ch: TimerCh) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().close(null, timerChip(ch));
}

fn timerStart(ctx: ?*anyopaque, ch: TimerCh) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().start(null, timerChip(ch));
}

fn timerStop(ctx: ?*anyopaque, ch: TimerCh) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().stop(null, timerChip(ch));
}

fn timerRead(ctx: ?*anyopaque, ch: TimerCh, out: *u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().read(null, timerChip(ch), out);
}

fn timerSetPeriod(ctx: ?*anyopaque, ch: TimerCh, period: u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().set_period(null, timerChip(ch), period);
}

fn timerCapture(ctx: ?*anyopaque, ch: TimerCh, out: *u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().capture_read(null, timerChip(ch), out);
}

fn timerTakeWrap(ctx: ?*anyopaque, ch: TimerCh, out: *bool) callconv(.c) ErrCode {
    _ = ctx;
    return chipTimer().take_wrap(null, timerChip(ch), out);
}

const timer_iface: gpt.TimerIface = .{
    .get_caps = timerCaps,
    .open = timerOpen,
    .close = timerClose,
    .start = timerStart,
    .stop = timerStop,
    .read = timerRead,
    .set_period = timerSetPeriod,
    .capture_read = timerCapture,
    .take_wrap = timerTakeWrap,
};

// -------------------------------------------------------------------- pwm --

fn pwmChip(ch: PwmCh) PwmCh {
    return .{ .index = pwm_rows[ch.index].chip };
}

fn chipPwm() *const gpt.PwmIface {
    return seam.fw_pwm_ra8_iface();
}

fn pwmCaps(ctx: ?*anyopaque, out: *gpt.PwmCaps) callconv(.c) ErrCode {
    _ = ctx;
    const err = chipPwm().get_caps(null, out);
    if (err == ok) out.channel_count = pwm_count;
    return err;
}

/// Route the pin first, so a refused pin never reaches the counter, and hand
/// it back if the adapter then refuses the open.
fn pwmOpen(ctx: ?*anyopaque, ch: PwmCh, period: u32, pol: u8) callconv(.c) ErrCode {
    _ = ctx;
    const row = pwm_rows[ch.index];
    const routed = seam.ra8_pfs_route_peripheral(row.pin, psel_gtioc, row.owner);
    if (routed != ok) return routed;
    const err = chipPwm().open(null, pwmChip(ch), period, pol);
    if (err != ok) _ = seam.ra8_pin_validator_release(row.pin);
    return err;
}

/// Release the pin only once the counter is closed.
fn pwmClose(ctx: ?*anyopaque, ch: PwmCh) callconv(.c) ErrCode {
    _ = ctx;
    const err = chipPwm().close(null, pwmChip(ch));
    if (err != ok) return err;
    return seam.ra8_pin_validator_release(pwm_rows[ch.index].pin);
}

fn pwmStart(ctx: ?*anyopaque, ch: PwmCh) callconv(.c) ErrCode {
    _ = ctx;
    return chipPwm().start(null, pwmChip(ch));
}

fn pwmStop(ctx: ?*anyopaque, ch: PwmCh) callconv(.c) ErrCode {
    _ = ctx;
    return chipPwm().stop(null, pwmChip(ch));
}

fn pwmSetPeriod(ctx: ?*anyopaque, ch: PwmCh, period: u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipPwm().set_period(null, pwmChip(ch), period);
}

fn pwmSetDuty(ctx: ?*anyopaque, ch: PwmCh, duty: u32) callconv(.c) ErrCode {
    _ = ctx;
    return chipPwm().set_duty(null, pwmChip(ch), duty);
}

const pwm_iface: gpt.PwmIface = .{
    .get_caps = pwmCaps,
    .open = pwmOpen,
    .close = pwmClose,
    .start = pwmStart,
    .stop = pwmStop,
    .set_period = pwmSetPeriod,
    .set_duty = pwmSetDuty,
};

// ---------------------------------------------------------------- handles --

/// Bind @p tmr to the board timer profile. The facade rejects a null handle.
pub fn bindTimer(tmr: ?*FwTimer) ErrCode {
    const dst = tmr orelse return invalid_arg;
    return seam.fw_timer_bind(dst, &timer_iface, null);
}

/// Bind @p pwm to the board PWM profile.
pub fn bindPwm(pwm: ?*FwPwm) ErrCode {
    const dst = pwm orelse return invalid_arg;
    return seam.fw_pwm_bind(dst, &pwm_iface, null);
}

var board_timer: FwTimer = .{};
var board_pwm: FwPwm = .{};

/// The board timer handle, bound on first use. Binding cannot fail: this
/// file's own storage and a fully populated ops struct are all the facade
/// checks (see `clock_profile.handle`).
pub fn timerHandle() *const FwTimer {
    if (!board_timer.bound) _ = bindTimer(&board_timer);
    return &board_timer;
}

/// The board PWM handle, bound on first use, for the same reason.
pub fn pwmHandle() *const FwPwm {
    if (!board_pwm.bound) _ = bindPwm(&board_pwm);
    return &board_pwm;
}
