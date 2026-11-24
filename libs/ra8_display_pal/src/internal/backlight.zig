//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure hardware-independent logic for display backlight brightness conversion
//! and the 3-state idle dimming policy (ACTIVE -> DIMMED -> OFF).
//! Keep free of HAL/board/MMIO imports so the portability gate compiles it
//! unchanged across host, ARM, and RISC-V targets.

const std = @import("std");
const core = @import("root.zig");

pub const mode_binary: u8 = 0;
pub const mode_pwm: u8 = 1;

pub const state_active: u8 = 0;
pub const state_dimmed: u8 = 1;
pub const state_off: u8 = 2;

pub const pwm_duty_full: u32 = 65536;
pub const pwm_pol_active_high: u8 = 1;
pub const pwm_pol_active_low: u8 = 2;

pub const err_ok: u16 = core.err_ok;
pub const err_invalid_arg: u16 = 0x103;
pub const err_invalid_state: u16 = 0x104;
pub const err_not_supported: u16 = 0x107;
pub const err_not_initialized: u16 = 0x10F;
pub const err_null_ptr: u16 = core.err_null_ptr;

/// `display_backlight_policy_cfg_t`.
pub const BacklightPolicyCfg = extern struct {
    dim_after_ms: u32,
    off_after_ms: u32,
    active_pct: u8,
    dimmed_pct: u8,
    supports_dim: u8,
    reserved: u8 = 0,
};

/// `display_backlight_policy_t`.
pub const BacklightPolicy = extern struct {
    cfg: BacklightPolicyCfg,
    last_activity_ms: u32,
    state: u8,
    current_pct: u8,
    initialized: u8,
    reserved: u8 = 0,
};

/// `display_backlight_policy_decision_t`.
pub const BacklightPolicyDecision = extern struct {
    state: u8,
    target_pct: u8,
    changed: u8,
    on: u8,
};

comptime {
    std.debug.assert(@sizeOf(BacklightPolicyCfg) == 12);
    std.debug.assert(@sizeOf(BacklightPolicy) == 20);
    std.debug.assert(@offsetOf(BacklightPolicy, "state") == 16);
    std.debug.assert(@sizeOf(BacklightPolicyDecision) == 4);
}

pub fn modeValid(mode: u8) bool {
    return mode <= mode_pwm;
}

pub fn pwmPolarityValid(polarity: u8) bool {
    return polarity == pwm_pol_active_high or polarity == pwm_pol_active_low;
}

/// Converts a 0..100 brightness percent into the rounded Q16 duty expected by
/// `fw_pwm_set_duty`. Returns null for > 100.
pub fn percentToQ16(percent: u8) ?u32 {
    if (percent > 100) return null;
    if (percent == 0) return 0;
    if (percent == 100) return pwm_duty_full;
    return (@as(u32, percent) * pwm_duty_full + 50) / 100;
}

/// Validates a policy configuration before initializing state.
/// Binary panels (`supports_dim == 0`) require `active_pct == 100` so intermediate
/// levels are rejected fail-closed rather than silently clamped to full ON.
pub fn validatePolicyCfg(cfg: *const BacklightPolicyCfg) u16 {
    if (cfg.off_after_ms == 0) return err_invalid_arg;
    if (cfg.dim_after_ms > cfg.off_after_ms) return err_invalid_arg;
    if (cfg.active_pct == 0 or cfg.active_pct > 100) return err_invalid_arg;
    if (cfg.supports_dim == 0) {
        if (cfg.active_pct != 100) return err_not_supported;
        return err_ok;
    }
    if (cfg.dim_after_ms > 0 and cfg.dim_after_ms < cfg.off_after_ms) {
        if (cfg.dimmed_pct == 0 or cfg.dimmed_pct > cfg.active_pct) return err_invalid_arg;
    }
    return err_ok;
}

pub fn initPolicy(policy: *BacklightPolicy, cfg: *const BacklightPolicyCfg, now_ms: u32) u16 {
    const err = validatePolicyCfg(cfg);
    if (err != err_ok) return err;
    policy.* = .{
        .cfg = .{
            .dim_after_ms = cfg.dim_after_ms,
            .off_after_ms = cfg.off_after_ms,
            .active_pct = cfg.active_pct,
            .dimmed_pct = cfg.dimmed_pct,
            .supports_dim = if (cfg.supports_dim != 0) 1 else 0,
            .reserved = 0,
        },
        .last_activity_ms = now_ms,
        .state = state_active,
        .current_pct = cfg.active_pct,
        .initialized = 1,
        .reserved = 0,
    };
    return err_ok;
}

pub fn recordActivity(policy: *BacklightPolicy, now_ms: u32) BacklightPolicyDecision {
    const was_active = policy.state == state_active and policy.current_pct == policy.cfg.active_pct;
    policy.last_activity_ms = now_ms;
    policy.state = state_active;
    policy.current_pct = policy.cfg.active_pct;
    return .{
        .state = state_active,
        .target_pct = policy.current_pct,
        .changed = if (was_active) 0 else 1,
        .on = 1,
    };
}

/// Evaluates idle timeouts using wrap-safe unsigned subtraction while keeping
/// idle state monotonic until `recordActivity` wakes it.
pub fn stepPolicy(policy: *BacklightPolicy, now_ms: u32) BacklightPolicyDecision {
    const elapsed_ms: u32 = now_ms -% policy.last_activity_ms;
    var next_state = policy.state;
    var next_pct = policy.current_pct;

    if (policy.state == state_off) {
        next_state = state_off;
        next_pct = 0;
    } else if (elapsed_ms >= policy.cfg.off_after_ms) {
        next_state = state_off;
        next_pct = 0;
    } else if (policy.state == state_active and
        policy.cfg.supports_dim != 0 and
        policy.cfg.dim_after_ms > 0 and
        elapsed_ms >= policy.cfg.dim_after_ms)
    {
        next_state = state_dimmed;
        next_pct = policy.cfg.dimmed_pct;
    }

    const changed = next_state != policy.state or next_pct != policy.current_pct;
    policy.state = next_state;
    policy.current_pct = next_pct;
    return .{
        .state = next_state,
        .target_pct = next_pct,
        .changed = if (changed) 1 else 0,
        .on = if (next_pct > 0) 1 else 0,
    };
}
