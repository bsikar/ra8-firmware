//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the display backlight controller and idle dimming policy
//! defined in `ra8_display_pal_backlight.h`.

const impl = @import("internal/backlight.zig");
const std = @import("std");

const tag_backlight: [*:0]const u8 = "display.backlight";
const tag_policy: [*:0]const u8 = "display.backlight_policy";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

pub const EnableFn = *const fn (?*anyopaque, u8) callconv(.c) u16;

/// Mirrors `fw_pwm_ch_t`.
pub const PwmCh = extern struct {
    index: u8,
};

/// Mirrors `fw_pwm_caps_t` (`has_active_low` uses `u8` to match C `_Bool` width).
pub const PwmCaps = extern struct {
    channel_count: u8,
    counter_bits: u8,
    period_max: u32,
    has_active_low: u8,
};

/// Mirrors `fw_pwm_iface_t`.
pub const PwmIface = extern struct {
    get_caps: ?*const fn (?*anyopaque, *PwmCaps) callconv(.c) u16,
    open: ?*const fn (?*anyopaque, PwmCh, u32, u8) callconv(.c) u16,
    close: ?*const fn (?*anyopaque, PwmCh) callconv(.c) u16,
    start: ?*const fn (?*anyopaque, PwmCh) callconv(.c) u16,
    stop: ?*const fn (?*anyopaque, PwmCh) callconv(.c) u16,
    set_period: ?*const fn (?*anyopaque, PwmCh, u32) callconv(.c) u16,
    set_duty: ?*const fn (?*anyopaque, PwmCh, u32) callconv(.c) u16,
};

/// Mirrors `fw_pwm_t` (`bound` uses `u8` to match C `_Bool` width).
pub const Pwm = extern struct {
    iface: ?*const PwmIface,
    ctx: ?*anyopaque,
    caps: PwmCaps,
    bound: u8,
};

/// Mirrors `display_backlight_cfg_t`.
pub const BacklightCfg = extern struct {
    pwm: ?*const Pwm,
    set_enable: ?EnableFn,
    ctx: ?*anyopaque,
    pwm_period_ticks: u32,
    mode: u8,
    pwm_ch: PwmCh,
    pwm_polarity: u8,
    reserved: u8 = 0,
};

/// Mirrors `display_backlight_t`.
pub const BacklightDev = extern struct {
    cfg: BacklightCfg,
    initialized: u8,
    enabled: u8,
    pwm_running: u8,
    brightness_pct: u8,
};

const ptr_bytes = @sizeOf(usize);
comptime {
    std.debug.assert(@offsetOf(BacklightCfg, "mode") == ptr_bytes * 3 + 4);
}

fn nullPtr(tag: [*:0]const u8, message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return impl.err_null_ptr;
}

fn validatePwmHandle(pwm: ?*const Pwm, ch: PwmCh) u16 {
    const handle = pwm orelse return nullPtr(tag_backlight, "cfg->pwm must not be nullptr");
    if (handle.bound == 0) return impl.err_not_initialized;
    const iface = handle.iface orelse return impl.err_invalid_arg;
    if (iface.open == null or
        iface.close == null or
        iface.start == null or
        iface.stop == null or
        iface.set_duty == null)
    {
        return impl.err_invalid_arg;
    }
    if (ch.index >= handle.caps.channel_count) return impl.err_invalid_arg;
    return impl.err_ok;
}

fn setPwmBrightness(dev: *BacklightDev, percent: u8) u16 {
    const pwm = dev.cfg.pwm orelse return impl.err_not_initialized;
    const iface = pwm.iface orelse return impl.err_not_initialized;
    const duty = impl.percentToQ16(percent) orelse return impl.err_invalid_arg;
    const set_duty_fn = iface.set_duty orelse return impl.err_invalid_arg;
    const start_fn = iface.start orelse return impl.err_invalid_arg;
    const stop_fn = iface.stop orelse return impl.err_invalid_arg;

    if (percent == 0) {
        const duty_err = set_duty_fn(pwm.ctx, dev.cfg.pwm_ch, 0);
        if (duty_err != impl.err_ok) return duty_err;
        if (dev.pwm_running != 0) {
            const stop_err = stop_fn(pwm.ctx, dev.cfg.pwm_ch);
            if (stop_err != impl.err_ok) return stop_err;
            dev.pwm_running = 0;
        }
        if (dev.cfg.set_enable) |gate_fn| {
            if (dev.enabled != 0) {
                const gate_err = gate_fn(dev.cfg.ctx, 0);
                if (gate_err != impl.err_ok) return gate_err;
                dev.enabled = 0;
            }
        }
        dev.brightness_pct = 0;
        return impl.err_ok;
    }

    if (dev.cfg.set_enable) |gate_fn| {
        if (dev.enabled == 0) {
            const gate_err = gate_fn(dev.cfg.ctx, 1);
            if (gate_err != impl.err_ok) return gate_err;
            dev.enabled = 1;
        }
    }
    const duty_err = set_duty_fn(pwm.ctx, dev.cfg.pwm_ch, duty);
    if (duty_err != impl.err_ok) return duty_err;
    if (dev.pwm_running == 0) {
        const start_err = start_fn(pwm.ctx, dev.cfg.pwm_ch);
        if (start_err != impl.err_ok) return start_err;
        dev.pwm_running = 1;
    }
    dev.brightness_pct = percent;
    return impl.err_ok;
}

pub export fn display_backlight_percent_to_q16(percent: u8, out_q16: ?*u32) callconv(.c) u16 {
    const dst = out_q16 orelse return nullPtr(tag_backlight, "out_q16 must not be nullptr");
    const duty = impl.percentToQ16(percent) orelse {
        ra8_log_emit_error(tag_backlight, "percent_to_q16: percent > 100");
        return impl.err_invalid_arg;
    };
    dst.* = duty;
    return impl.err_ok;
}

pub export fn display_backlight_init(dev: ?*BacklightDev, cfg: ?*const BacklightCfg) callconv(.c) u16 {
    const d = dev orelse return nullPtr(tag_backlight, "dev must not be nullptr");
    const c = cfg orelse return nullPtr(tag_backlight, "cfg must not be nullptr");
    if (!impl.modeValid(c.mode)) {
        ra8_log_emit_error(tag_backlight, "init: invalid backlight mode");
        return impl.err_invalid_arg;
    }

    if (c.mode == impl.mode_binary) {
        const gate_fn = c.set_enable orelse return nullPtr(tag_backlight, "cfg->set_enable must not be nullptr");
        const err = gate_fn(c.ctx, 0);
        if (err != impl.err_ok) return err;
        d.* = .{
            .cfg = c.*,
            .initialized = 1,
            .enabled = 0,
            .pwm_running = 0,
            .brightness_pct = 0,
        };
        return impl.err_ok;
    }

    const pwm_err = validatePwmHandle(c.pwm, c.pwm_ch);
    if (pwm_err != impl.err_ok) return pwm_err;
    const pwm = c.pwm.?;
    if (!impl.pwmPolarityValid(c.pwm_polarity)) return impl.err_invalid_arg;
    if (c.pwm_polarity == impl.pwm_pol_active_low and pwm.caps.has_active_low == 0) {
        return impl.err_not_supported;
    }
    if (c.pwm_period_ticks == 0 or c.pwm_period_ticks > pwm.caps.period_max) {
        return impl.err_invalid_arg;
    }

    const open_err = pwm.iface.?.open.?(pwm.ctx, c.pwm_ch, c.pwm_period_ticks, c.pwm_polarity);
    if (open_err != impl.err_ok) return open_err;

    if (c.set_enable) |gate_fn| {
        const gate_err = gate_fn(c.ctx, 0);
        if (gate_err != impl.err_ok) {
            _ = pwm.iface.?.close.?(pwm.ctx, c.pwm_ch);
            return gate_err;
        }
    }

    d.* = .{
        .cfg = c.*,
        .initialized = 1,
        .enabled = 0,
        .pwm_running = 0,
        .brightness_pct = 0,
    };
    return impl.err_ok;
}

pub export fn display_backlight_deinit(dev: ?*BacklightDev) callconv(.c) u16 {
    const d = dev orelse return nullPtr(tag_backlight, "dev must not be nullptr");
    if (d.initialized == 0) return impl.err_not_initialized;

    var first_err: u16 = impl.err_ok;
    if (d.cfg.mode == impl.mode_pwm) {
        if (d.cfg.pwm) |pwm| {
            if (pwm.iface) |iface| {
                if (d.pwm_running != 0) {
                    if (iface.stop) |stop_fn| {
                        const err = stop_fn(pwm.ctx, d.cfg.pwm_ch);
                        if (first_err == impl.err_ok and err != impl.err_ok) first_err = err;
                    }
                }
                if (iface.close) |close_fn| {
                    const err = close_fn(pwm.ctx, d.cfg.pwm_ch);
                    if (first_err == impl.err_ok and err != impl.err_ok) first_err = err;
                }
            }
        }
    }

    if (d.cfg.set_enable) |gate_fn| {
        const err = gate_fn(d.cfg.ctx, 0);
        if (first_err == impl.err_ok and err != impl.err_ok) first_err = err;
    }

    d.initialized = 0;
    d.enabled = 0;
    d.pwm_running = 0;
    d.brightness_pct = 0;
    return first_err;
}

pub export fn display_backlight_set_brightness(dev: ?*BacklightDev, percent: u8) callconv(.c) u16 {
    const d = dev orelse return nullPtr(tag_backlight, "dev must not be nullptr");
    if (d.initialized == 0) return impl.err_not_initialized;
    if (percent > 100) {
        ra8_log_emit_error(tag_backlight, "set_brightness: percent > 100");
        return impl.err_invalid_arg;
    }

    if (d.cfg.mode == impl.mode_binary) {
        if (percent != 0 and percent != 100) {
            return impl.err_not_supported;
        }
        const gate_fn = d.cfg.set_enable orelse return impl.err_not_initialized;
        const level: u8 = if (percent == 100) 1 else 0;
        const err = gate_fn(d.cfg.ctx, level);
        if (err != impl.err_ok) return err;
        d.enabled = level;
        d.brightness_pct = percent;
        return impl.err_ok;
    }

    return setPwmBrightness(d, percent);
}

pub export fn display_backlight_apply_decision(
    dev: ?*BacklightDev,
    decision: ?*const impl.BacklightPolicyDecision,
) callconv(.c) u16 {
    const d = dev orelse return nullPtr(tag_backlight, "dev must not be nullptr");
    const dec = decision orelse return nullPtr(tag_backlight, "decision must not be nullptr");
    if (d.initialized == 0) return impl.err_not_initialized;
    if (dec.state > impl.state_off or dec.target_pct > 100) return impl.err_invalid_arg;
    if (dec.changed == 0) return impl.err_ok;
    return display_backlight_set_brightness(d, dec.target_pct);
}

pub export fn display_backlight_policy_init(
    policy: ?*impl.BacklightPolicy,
    cfg: ?*const impl.BacklightPolicyCfg,
    now_ms: u32,
) callconv(.c) u16 {
    const p = policy orelse return nullPtr(tag_policy, "policy must not be nullptr");
    const c = cfg orelse return nullPtr(tag_policy, "cfg must not be nullptr");
    const err = impl.initPolicy(p, c, now_ms);
    if (err != impl.err_ok) {
        ra8_log_emit_error(tag_policy, "policy_init: invalid configuration");
        return err;
    }
    return impl.err_ok;
}

pub export fn display_backlight_policy_activity(
    policy: ?*impl.BacklightPolicy,
    now_ms: u32,
    out: ?*impl.BacklightPolicyDecision,
) callconv(.c) u16 {
    const p = policy orelse return nullPtr(tag_policy, "policy must not be nullptr");
    const dst = out orelse return nullPtr(tag_policy, "out must not be nullptr");
    if (p.initialized == 0) return impl.err_not_initialized;
    dst.* = impl.recordActivity(p, now_ms);
    return impl.err_ok;
}

pub export fn display_backlight_policy_step(
    policy: ?*impl.BacklightPolicy,
    now_ms: u32,
    out: ?*impl.BacklightPolicyDecision,
) callconv(.c) u16 {
    const p = policy orelse return nullPtr(tag_policy, "policy must not be nullptr");
    const dst = out orelse return nullPtr(tag_policy, "out must not be nullptr");
    if (p.initialized == 0) return impl.err_not_initialized;
    dst.* = impl.stepPolicy(p, now_ms);
    return impl.err_ok;
}
