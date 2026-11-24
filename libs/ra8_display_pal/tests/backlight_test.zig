//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native unit tests for pure backlight brightness conversion and the
//! 3-state idle dimming policy (ACTIVE -> DIMMED -> OFF).

const std = @import("std");
const impl = @import("backlight");

test "percentToQ16 converts endpoints cleanly and rejects > 100" {
    try std.testing.expectEqual(@as(?u32, 0), impl.percentToQ16(0));
    try std.testing.expectEqual(@as(?u32, 16384), impl.percentToQ16(25));
    try std.testing.expectEqual(@as(?u32, 32768), impl.percentToQ16(50));
    try std.testing.expectEqual(@as(?u32, 65536), impl.percentToQ16(100));
    try std.testing.expectEqual(@as(?u32, null), impl.percentToQ16(101));
    try std.testing.expectEqual(@as(?u32, null), impl.percentToQ16(255));
}

test "validatePolicyCfg enforces bounds and fails closed on binary partial active pct" {
    const base = impl.BacklightPolicyCfg{
        .dim_after_ms = 10000,
        .off_after_ms = 30000,
        .active_pct = 100,
        .dimmed_pct = 25,
        .supports_dim = 1,
    };
    try std.testing.expectEqual(impl.err_ok, impl.validatePolicyCfg(&base));

    var zero_off = base;
    zero_off.off_after_ms = 0;
    try std.testing.expectEqual(impl.err_invalid_arg, impl.validatePolicyCfg(&zero_off));

    var dim_after_off = base;
    dim_after_off.dim_after_ms = 30001;
    try std.testing.expectEqual(impl.err_invalid_arg, impl.validatePolicyCfg(&dim_after_off));

    var zero_active = base;
    zero_active.active_pct = 0;
    try std.testing.expectEqual(impl.err_invalid_arg, impl.validatePolicyCfg(&zero_active));

    var dimmed_over_active = base;
    dimmed_over_active.active_pct = 60;
    dimmed_over_active.dimmed_pct = 61;
    try std.testing.expectEqual(impl.err_invalid_arg, impl.validatePolicyCfg(&dimmed_over_active));

    var binary_partial = base;
    binary_partial.supports_dim = 0;
    binary_partial.active_pct = 80;
    try std.testing.expectEqual(impl.err_not_supported, impl.validatePolicyCfg(&binary_partial));

    var binary_ok = base;
    binary_ok.supports_dim = 0;
    try std.testing.expectEqual(impl.err_ok, impl.validatePolicyCfg(&binary_ok));
}

test "PWM policy transitions ACTIVE -> DIMMED -> OFF -> ACTIVE across tick wrap" {
    var policy: impl.BacklightPolicy = undefined;
    const cfg = impl.BacklightPolicyCfg{
        .dim_after_ms = 5000,
        .off_after_ms = 12000,
        .active_pct = 90,
        .dimmed_pct = 20,
        .supports_dim = 1,
    };
    const t0: u32 = 0xFFFF_F000;
    try std.testing.expectEqual(impl.err_ok, impl.initPolicy(&policy, &cfg, t0));
    try std.testing.expectEqual(impl.state_active, policy.state);
    try std.testing.expectEqual(@as(u8, 90), policy.current_pct);

    const still_active = impl.stepPolicy(&policy, t0 +% 4999);
    try std.testing.expectEqual(impl.state_active, still_active.state);
    try std.testing.expectEqual(@as(u8, 0), still_active.changed);

    const dimmed = impl.stepPolicy(&policy, t0 +% 5000);
    try std.testing.expectEqual(impl.state_dimmed, dimmed.state);
    try std.testing.expectEqual(@as(u8, 20), dimmed.target_pct);
    try std.testing.expectEqual(@as(u8, 1), dimmed.changed);
    try std.testing.expectEqual(@as(u8, 1), dimmed.on);

    const still_dimmed = impl.stepPolicy(&policy, t0 +% 11999);
    try std.testing.expectEqual(impl.state_dimmed, still_dimmed.state);
    try std.testing.expectEqual(@as(u8, 0), still_dimmed.changed);

    const off = impl.stepPolicy(&policy, t0 +% 12000);
    try std.testing.expectEqual(impl.state_off, off.state);
    try std.testing.expectEqual(@as(u8, 0), off.target_pct);
    try std.testing.expectEqual(@as(u8, 1), off.changed);
    try std.testing.expectEqual(@as(u8, 0), off.on);

    const woken = impl.recordActivity(&policy, t0 +% 13000);
    try std.testing.expectEqual(impl.state_active, woken.state);
    try std.testing.expectEqual(@as(u8, 90), woken.target_pct);
    try std.testing.expectEqual(@as(u8, 1), woken.changed);
    try std.testing.expectEqual(@as(u8, 1), woken.on);

    const redundant_wake = impl.recordActivity(&policy, t0 +% 13500);
    try std.testing.expectEqual(@as(u8, 0), redundant_wake.changed);
}

test "binary policy skips DIMMED stage and turns OFF cleanly at off_after_ms" {
    var policy: impl.BacklightPolicy = undefined;
    const cfg = impl.BacklightPolicyCfg{
        .dim_after_ms = 10000,
        .off_after_ms = 30000,
        .active_pct = 100,
        .dimmed_pct = 20,
        .supports_dim = 0,
    };
    try std.testing.expectEqual(impl.err_ok, impl.initPolicy(&policy, &cfg, 100));

    const mid = impl.stepPolicy(&policy, 25000);
    try std.testing.expectEqual(impl.state_active, mid.state);
    try std.testing.expectEqual(@as(u8, 100), mid.target_pct);
    try std.testing.expectEqual(@as(u8, 0), mid.changed);

    const off = impl.stepPolicy(&policy, 30100);
    try std.testing.expectEqual(impl.state_off, off.state);
    try std.testing.expectEqual(@as(u8, 0), off.target_pct);
    try std.testing.expectEqual(@as(u8, 1), off.changed);
}
