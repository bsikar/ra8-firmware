//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Accumulator-core tests. The C suite in
//! `tests/misc/src/test_ra8_power_profile.c` stays the behavioural contract;
//! these cover the bookkeeping edges directly.

const std = @import("std");
const implementation = @import("implementation");

test "enter stamps the open timestamp and counts the entry" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(0, 1_000);

    const slot = profiler.stats.regions[0];
    try std.testing.expectEqual(@as(u64, 1), slot.entries);
    try std.testing.expectEqual(@as(u64, 1_000), slot.last_enter_us);
    try std.testing.expect(slot.is_open);
}

test "exit closes the span and folds the delta into the total" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(2, 500);
    try std.testing.expectEqual(implementation.ExitOutcome.closed, profiler.markExit(2, 850));

    const slot = profiler.stats.regions[2];
    try std.testing.expectEqual(@as(u64, 350), slot.total_time_us);
    try std.testing.expectEqual(@as(u64, 1), slot.exits);
    try std.testing.expect(!slot.is_open);
}

test "exit without a matching enter is reported and accumulates nothing" {
    var profiler: implementation.Profiler = .{};
    try std.testing.expectEqual(implementation.ExitOutcome.unmatched, profiler.markExit(4, 900));

    const slot = profiler.stats.regions[4];
    try std.testing.expectEqual(@as(u64, 0), slot.entries);
    try std.testing.expectEqual(@as(u64, 1), slot.exits);
    try std.testing.expectEqual(@as(u64, 0), slot.total_time_us);
}

test "a second enter overwrites the open stamp but keeps counting entries" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(1, 100);
    profiler.markEnter(1, 400);
    try std.testing.expectEqual(implementation.ExitOutcome.closed, profiler.markExit(1, 600));

    const slot = profiler.stats.regions[1];
    try std.testing.expectEqual(@as(u64, 2), slot.entries);
    try std.testing.expectEqual(@as(u64, 1), slot.exits);
    try std.testing.expectEqual(@as(u64, 200), slot.total_time_us);
}

test "a backwards clock contributes nothing instead of wrapping" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(3, 1_000);
    try std.testing.expectEqual(implementation.ExitOutcome.closed, profiler.markExit(3, 900));

    const slot = profiler.stats.regions[3];
    try std.testing.expectEqual(@as(u64, 0), slot.total_time_us);
    try std.testing.expect(!slot.is_open);
}

test "spans accumulate across repeated pairs" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(0, 0);
    _ = profiler.markExit(0, 150);
    profiler.markEnter(0, 1_000);
    _ = profiler.markExit(0, 1_200);

    try std.testing.expectEqual(@as(u64, 350), profiler.stats.regions[0].total_time_us);
}

test "regions are independent" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(0, 10);
    profiler.markEnter(5, 20);
    _ = profiler.markExit(0, 40);

    try std.testing.expectEqual(@as(u64, 30), profiler.stats.regions[0].total_time_us);
    try std.testing.expect(profiler.stats.regions[5].is_open);
    try std.testing.expectEqual(@as(u64, 0), profiler.stats.regions[5].total_time_us);
}

test "reset clears every accumulator" {
    var profiler: implementation.Profiler = .{};
    profiler.markEnter(7, 10);
    _ = profiler.markExit(7, 60);
    profiler.reset();

    for (profiler.stats.regions) |slot| {
        try std.testing.expectEqual(@as(u64, 0), slot.entries);
        try std.testing.expectEqual(@as(u64, 0), slot.exits);
        try std.testing.expectEqual(@as(u64, 0), slot.total_time_us);
        try std.testing.expect(!slot.is_open);
    }
}

test "range predicate matches the static array bound" {
    try std.testing.expect(implementation.Profiler.inRange(0));
    try std.testing.expect(implementation.Profiler.inRange(implementation.max_regions - 1));
    try std.testing.expect(!implementation.Profiler.inRange(implementation.max_regions));
    try std.testing.expect(!implementation.Profiler.inRange(255));
}
