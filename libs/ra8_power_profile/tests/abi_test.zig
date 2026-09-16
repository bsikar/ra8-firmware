//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI-membrane tests: the exported entry points, their `ra8_err_t` values,
//! and the hook dispatch, exercised the way `inc/ra8_power_profile.h`
//! documents them.

const std = @import("std");
const abi = @import("abi");

/// The membrane logs through `ra8_log_emit_error`, which lives in ra8_core.
/// The Zig test binary links neither ra8_core nor the C harness, so stand the
/// symbol up here and let the C suite cover the real logging path.
export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
}

const ok = @intFromEnum(abi.ProfileError.ok);
const invalid_state = @intFromEnum(abi.ProfileError.invalid_state);
const not_initialized = @intFromEnum(abi.ProfileError.not_initialized);
const range_check_failed = @intFromEnum(abi.ProfileError.range_check_failed);
const null_ptr = @intFromEnum(abi.ProfileError.null_ptr);

/// Mock GPIO log, standing in for the C suite's `s_gpio`.
const PulseLog = struct {
    var count: usize = 0;
    var regions: [8]u8 = @splat(0);
    var entering: [8]bool = @splat(false);

    fn clear() void {
        count = 0;
    }

    fn record(ctx: ?*anyopaque, region_id: u8, is_entering: bool) callconv(.c) void {
        _ = ctx;
        if (count < regions.len) {
            regions[count] = region_id;
            entering[count] = is_entering;
        }
        count += 1;
    }
};

/// Mock clock, standing in for the C suite's injected RTC.
const Clock = struct {
    var value: u64 = 0;

    fn read(ctx: ?*anyopaque) callconv(.c) u64 {
        _ = ctx;
        return value;
    }
};

fn initWithMocks() void {
    PulseLog.clear();
    Clock.value = 0;
    var cfg: abi.Config = .{ .pulse = PulseLog.record, .now_us = Clock.read };
    std.debug.assert(abi.ra8_power_profile_init(&cfg) == ok);
}

test "init rejects a null config" {
    abi.testOnlyTeardown();
    try std.testing.expectEqual(null_ptr, abi.ra8_power_profile_init(null));
}

test "every entry point rejects calls before init" {
    abi.testOnlyTeardown();
    var stats: abi.Stats = .{};
    try std.testing.expectEqual(not_initialized, abi.ra8_power_profile_mark_enter(0));
    try std.testing.expectEqual(not_initialized, abi.ra8_power_profile_mark_exit(0));
    try std.testing.expectEqual(not_initialized, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(not_initialized, abi.ra8_power_profile_reset_stats());
}

test "init zeroes every accumulator" {
    abi.testOnlyTeardown();
    initWithMocks();

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    for (stats.regions) |slot| {
        try std.testing.expectEqual(@as(u64, 0), slot.entries);
        try std.testing.expectEqual(@as(u64, 0), slot.exits);
        try std.testing.expectEqual(@as(u64, 0), slot.total_time_us);
        try std.testing.expect(!slot.is_open);
    }
}

test "enter and exit pulse the gpio hook once each, in order" {
    abi.testOnlyTeardown();
    initWithMocks();

    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(0));
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(0));

    try std.testing.expectEqual(@as(usize, 2), PulseLog.count);
    try std.testing.expectEqual(@as(u8, 0), PulseLog.regions[0]);
    try std.testing.expect(PulseLog.entering[0]);
    try std.testing.expectEqual(@as(u8, 0), PulseLog.regions[1]);
    try std.testing.expect(!PulseLog.entering[1]);
}

test "time accumulates across pairs through the clock hook" {
    abi.testOnlyTeardown();
    initWithMocks();

    Clock.value = 100;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(1));
    Clock.value = 250;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(1));
    Clock.value = 1_000;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(1));
    Clock.value = 1_200;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(1));

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(@as(u64, 2), stats.regions[1].entries);
    try std.testing.expectEqual(@as(u64, 2), stats.regions[1].exits);
    try std.testing.expectEqual(@as(u64, 350), stats.regions[1].total_time_us);
    try std.testing.expect(!stats.regions[1].is_open);
}

test "exit without enter reports invalid state and still pulses" {
    abi.testOnlyTeardown();
    initWithMocks();

    try std.testing.expectEqual(invalid_state, abi.ra8_power_profile_mark_exit(4));
    try std.testing.expectEqual(@as(usize, 1), PulseLog.count);
    try std.testing.expect(!PulseLog.entering[0]);

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(@as(u64, 0), stats.regions[4].entries);
    try std.testing.expectEqual(@as(u64, 1), stats.regions[4].exits);
}

test "out-of-range region ids are rejected on both edges" {
    abi.testOnlyTeardown();
    initWithMocks();

    try std.testing.expectEqual(range_check_failed, abi.ra8_power_profile_mark_enter(abi.max_regions));
    try std.testing.expectEqual(range_check_failed, abi.ra8_power_profile_mark_exit(255));
    try std.testing.expectEqual(@as(usize, 0), PulseLog.count);
}

test "get_stats rejects a null destination" {
    abi.testOnlyTeardown();
    initWithMocks();
    try std.testing.expectEqual(null_ptr, abi.ra8_power_profile_get_stats(null));
}

test "reset_stats clears accumulators and keeps the hooks" {
    abi.testOnlyTeardown();
    initWithMocks();

    Clock.value = 10;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(2));
    Clock.value = 60;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(2));
    try std.testing.expectEqual(ok, abi.ra8_power_profile_reset_stats());

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(@as(u64, 0), stats.regions[2].entries);
    try std.testing.expectEqual(@as(u64, 0), stats.regions[2].total_time_us);

    PulseLog.clear();
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(2));
    try std.testing.expectEqual(@as(usize, 1), PulseLog.count);
}

test "null hooks leave the profiler a pure software accumulator" {
    abi.testOnlyTeardown();
    var cfg: abi.Config = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_init(&cfg));

    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(3));
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(3));

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(@as(u64, 1), stats.regions[3].entries);
    try std.testing.expectEqual(@as(u64, 1), stats.regions[3].exits);
    try std.testing.expectEqual(@as(u64, 0), stats.regions[3].total_time_us);
}

test "regions stay independent across the ABI" {
    abi.testOnlyTeardown();
    initWithMocks();

    Clock.value = 10;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(0));
    Clock.value = 20;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_enter(5));
    Clock.value = 40;
    try std.testing.expectEqual(ok, abi.ra8_power_profile_mark_exit(0));

    var stats: abi.Stats = .{};
    try std.testing.expectEqual(ok, abi.ra8_power_profile_get_stats(&stats));
    try std.testing.expectEqual(@as(u64, 30), stats.regions[0].total_time_us);
    try std.testing.expect(stats.regions[5].is_open);
    try std.testing.expectEqual(@as(u64, 0), stats.regions[5].total_time_us);
}
