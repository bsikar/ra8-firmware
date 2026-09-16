//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_power_profile/inc/ra8_power_profile.h`. The
//! accumulator core lives in `internal/root.zig`; this file owns the
//! module-static state the C implementation kept in `s_cfg` / `s_stats` /
//! `s_initialized`, the hook dispatch, the `ra8_err_t` mapping, and the
//! diagnostic log lines `RA8_CHECK_NULL_PTR` and `RA8_VALIDATE_INIT` emitted.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Accumulator types of the published ABI.
pub const RegionStats = implementation.RegionStats;
/// Aggregate snapshot type of the published ABI.
pub const Stats = implementation.Stats;
/// Capacity of the accumulator array (`k_ra8_power_profile_max_regions`).
pub const max_regions = implementation.max_regions;

/// Subset of `ra8_err_t` this library returns.
pub const ProfileError = enum(u16) {
    ok = 0,
    invalid_state = 0x104,
    not_initialized = 0x10F,
    range_check_failed = 0x503,
    null_ptr = 0x504,
};

/// GPIO edge hook (`ra8_power_profile_gpio_pulse_fn_t`).
pub const PulseFn = *const fn (ctx: ?*anyopaque, region_id: u8, entering: bool) callconv(.c) void;
/// Microsecond clock hook (`ra8_power_profile_now_us_fn_t`).
pub const NowUsFn = *const fn (ctx: ?*anyopaque) callconv(.c) u64;

/// Initialisation configuration, laid out exactly as
/// `ra8_power_profile_config_t`.
pub const Config = extern struct {
    pulse: ?PulseFn = null,
    now_us: ?NowUsFn = null,
    user_ctx_gpio: ?*anyopaque = null,
    user_ctx_time: ?*anyopaque = null,
};

/// Component tag for diagnostic logging, matching the C `RA8_POWER_PROFILE_TAG`.
const tag: [*:0]const u8 = "PWRPROF";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

var state: implementation.Profiler = .{};
var config: Config = .{};
var initialized: bool = false;

comptime {
    if (@sizeOf(ProfileError) != 2) @compileError("ra8_power_profile error width");
    if (@intFromEnum(ProfileError.ok) != 0) @compileError("ra8_power_profile success value");
    if (@intFromEnum(ProfileError.invalid_state) != 0x104) @compileError("ra8_power_profile invalid-state value");
    if (@intFromEnum(ProfileError.not_initialized) != 0x10F) @compileError("ra8_power_profile not-initialized value");
    if (@intFromEnum(ProfileError.range_check_failed) != 0x503) @compileError("ra8_power_profile range-check value");
    if (@intFromEnum(ProfileError.null_ptr) != 0x504) @compileError("ra8_power_profile null-pointer value");

    if (@sizeOf(RegionStats) != 40) @compileError("ra8_power_profile_region_stats_t size");
    if (@alignOf(RegionStats) != 8) @compileError("ra8_power_profile_region_stats_t alignment");
    if (@offsetOf(RegionStats, "entries") != 0) @compileError("region stats entries offset");
    if (@offsetOf(RegionStats, "exits") != 8) @compileError("region stats exits offset");
    if (@offsetOf(RegionStats, "total_time_us") != 16) @compileError("region stats total_time_us offset");
    if (@offsetOf(RegionStats, "last_enter_us") != 24) @compileError("region stats last_enter_us offset");
    if (@offsetOf(RegionStats, "is_open") != 32) @compileError("region stats is_open offset");

    if (@sizeOf(Stats) != 40 * 16) @compileError("ra8_power_profile_stats_t size");
    if (max_regions != 16) @compileError("k_ra8_power_profile_max_regions");

    if (@offsetOf(Config, "pulse") != 0) @compileError("config pulse offset");
    if (@offsetOf(Config, "now_us") != @sizeOf(usize)) @compileError("config now_us offset");
    if (@offsetOf(Config, "user_ctx_gpio") != 2 * @sizeOf(usize)) @compileError("config user_ctx_gpio offset");
    if (@offsetOf(Config, "user_ctx_time") != 3 * @sizeOf(usize)) @compileError("config user_ctx_time offset");
    if (@sizeOf(Config) != 4 * @sizeOf(usize)) @compileError("ra8_power_profile_config_t size");
}

/// Current timestamp, or 0 when no clock hook was configured.
fn nowUs() u64 {
    const hook = config.now_us orelse return 0;
    return hook(config.user_ctx_time);
}

/// Fire the GPIO hook when one was configured.
fn firePulse(region_id: u8, entering: bool) void {
    const hook = config.pulse orelse return;
    hook(config.user_ctx_gpio, region_id, entering);
}

/// Shared bounds check, so the out-of-range log line has one source.
fn validateRegion(region_id: u8) ProfileError {
    if (!implementation.Profiler.inRange(region_id)) {
        ra8_log_emit_error(tag, "region id out of range");
        return .range_check_failed;
    }
    return .ok;
}

/// Shared init guard, mirroring `RA8_VALIDATE_INIT`.
fn requireInitialized() ProfileError {
    if (!initialized) {
        ra8_log_emit_error(tag, "init not called");
        return .not_initialized;
    }
    return .ok;
}

/// `ra8_power_profile_init`
pub export fn ra8_power_profile_init(cfg: ?*const Config) callconv(.c) u16 {
    const source = cfg orelse {
        ra8_log_emit_error(tag, "cfg must not be nullptr");
        return @intFromEnum(ProfileError.null_ptr);
    };
    config = source.*;
    state.reset();
    initialized = true;
    return @intFromEnum(ProfileError.ok);
}

/// `ra8_power_profile_mark_enter`
pub export fn ra8_power_profile_mark_enter(region_id: u8) callconv(.c) u16 {
    const init_err = requireInitialized();
    if (init_err != .ok) return @intFromEnum(init_err);
    const range_err = validateRegion(region_id);
    if (range_err != .ok) return @intFromEnum(range_err);

    state.markEnter(region_id, nowUs());
    firePulse(region_id, true);
    return @intFromEnum(ProfileError.ok);
}

/// `ra8_power_profile_mark_exit`
pub export fn ra8_power_profile_mark_exit(region_id: u8) callconv(.c) u16 {
    const init_err = requireInitialized();
    if (init_err != .ok) return @intFromEnum(init_err);
    const range_err = validateRegion(region_id);
    if (range_err != .ok) return @intFromEnum(range_err);

    const outcome = state.markExit(region_id, nowUs());
    const result: ProfileError = switch (outcome) {
        .closed => .ok,
        .unmatched => blk: {
            ra8_log_emit_error(tag, "exit without matching enter");
            break :blk .invalid_state;
        },
    };

    firePulse(region_id, false);
    return @intFromEnum(result);
}

/// `ra8_power_profile_get_stats`
pub export fn ra8_power_profile_get_stats(out_stats: ?*Stats) callconv(.c) u16 {
    const init_err = requireInitialized();
    if (init_err != .ok) return @intFromEnum(init_err);
    const destination = out_stats orelse {
        ra8_log_emit_error(tag, "out_stats must not be nullptr");
        return @intFromEnum(ProfileError.null_ptr);
    };
    destination.* = state.stats;
    return @intFromEnum(ProfileError.ok);
}

/// `ra8_power_profile_reset_stats`
pub export fn ra8_power_profile_reset_stats() callconv(.c) u16 {
    const init_err = requireInitialized();
    if (init_err != .ok) return @intFromEnum(init_err);
    state.reset();
    return @intFromEnum(ProfileError.ok);
}

/// Test-only reset of the module-static state, so a Zig test can observe the
/// uninitialised branches the C tests reach by linking a fresh binary.
pub fn testOnlyTeardown() void {
    state.reset();
    config = .{};
    initialized = false;
}
