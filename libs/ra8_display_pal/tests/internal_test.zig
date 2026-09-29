//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure decision logic of the display PAL's
//! backend-agnostic half: the dispatcher's handle judgement and the page-turn
//! refresh cadence.

const std = @import("std");
const implementation = @import("implementation");

test "handle judgement keeps the C's three-check order" {
    try std.testing.expectEqual(
        implementation.HandleStatus.null_handle,
        implementation.handleStatus(true, true, true),
    );
    try std.testing.expectEqual(
        implementation.HandleStatus.not_the_live_handle,
        implementation.handleStatus(false, false, true),
    );
    try std.testing.expectEqual(
        implementation.HandleStatus.not_the_live_handle,
        implementation.handleStatus(false, true, false),
    );
    try std.testing.expectEqual(
        implementation.HandleStatus.ok,
        implementation.handleStatus(false, true, true),
    );
}

test "handle judgement maps onto the C error codes" {
    try std.testing.expectEqual(
        implementation.err_null_ptr,
        implementation.HandleStatus.null_handle.code(),
    );
    try std.testing.expectEqual(
        implementation.err_invalid_arg,
        implementation.HandleStatus.not_the_live_handle.code(),
    );
    try std.testing.expectEqual(implementation.err_ok, implementation.HandleStatus.ok.code());
}

test "clean cadence clamps onto the documented bounds" {
    try std.testing.expectEqual(implementation.clean_every_min, implementation.clampCleanEvery(0));
    try std.testing.expectEqual(implementation.clean_every_min, implementation.clampCleanEvery(1));
    try std.testing.expectEqual(@as(u16, 8), implementation.clampCleanEvery(8));
    try std.testing.expectEqual(implementation.clean_every_max, implementation.clampCleanEvery(256));
    try std.testing.expectEqual(implementation.clean_every_max, implementation.clampCleanEvery(9999));
    try std.testing.expectEqual(implementation.clean_every_max, implementation.clampCleanEvery(65535));
}

test "range gates compare against the highest enumerator" {
    try std.testing.expect(implementation.policyKindInRange(implementation.policy_fast_only));
    try std.testing.expect(implementation.policyKindInRange(implementation.policy_quality));
    try std.testing.expect(implementation.policyKindInRange(implementation.policy_fast_clean));
    try std.testing.expect(!implementation.policyKindInRange(3));
    try std.testing.expect(!implementation.policyKindInRange(99));

    try std.testing.expect(implementation.turnEventInRange(implementation.event_open));
    try std.testing.expect(implementation.turnEventInRange(implementation.event_chapter));
    try std.testing.expect(!implementation.turnEventInRange(3));
    try std.testing.expect(!implementation.turnEventInRange(9));
}

test "fast/clean decision covers both conditions independently" {
    try std.testing.expect(!implementation.fastCleanTurn(0, 8, implementation.event_turn));
    try std.testing.expect(implementation.fastCleanTurn(7, 8, implementation.event_turn));
    try std.testing.expect(implementation.fastCleanTurn(0, 8, implementation.event_chapter));
    try std.testing.expect(implementation.fastCleanTurn(7, 8, implementation.event_chapter));
}

test "the counter increment wraps the way the C's uint16_t did" {
    try std.testing.expect(!implementation.fastCleanTurn(65535, 8, implementation.event_turn));
    try std.testing.expect(implementation.fastCleanTurn(65535, 8, implementation.event_chapter));

    var policy: implementation.Policy = .{
        .kind = implementation.policy_fast_clean,
        .clean_every = 8,
        .turns_since_clean = 65535,
    };
    const decision = implementation.decideFastClean(&policy, implementation.event_turn);
    try std.testing.expectEqual(implementation.refresh_fast, decision.hint);
    try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
}

test "a clean turn resets the counter and asks for quality" {
    var policy: implementation.Policy = .{
        .kind = implementation.policy_fast_clean,
        .clean_every = 4,
        .turns_since_clean = 3,
    };
    const decision = implementation.decideFastClean(&policy, implementation.event_turn);
    try std.testing.expectEqual(implementation.refresh_quality, decision.hint);
    try std.testing.expect(decision.full_update);
    try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
}

test "an open event clears the panel whatever the strategy is" {
    const kinds = [_]u8{
        implementation.policy_fast_only,
        implementation.policy_quality,
        implementation.policy_fast_clean,
    };
    for (kinds) |kind| {
        var policy: implementation.Policy = .{
            .kind = kind,
            .clean_every = 8,
            .turns_since_clean = 5,
        };
        const decision = implementation.decide(&policy, implementation.event_open);
        try std.testing.expectEqual(implementation.refresh_init, decision.hint);
        try std.testing.expect(decision.full_update);
        try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
    }
}

test "fast_only ignores the chapter boundary, quality ignores the counter" {
    var fast_only: implementation.Policy = .{
        .kind = implementation.policy_fast_only,
        .clean_every = 2,
        .turns_since_clean = 0,
    };
    const fast_decision = implementation.decide(&fast_only, implementation.event_chapter);
    try std.testing.expectEqual(implementation.refresh_fast, fast_decision.hint);
    try std.testing.expect(!fast_decision.full_update);
    try std.testing.expectEqual(@as(u16, 0), fast_only.turns_since_clean);

    var quality: implementation.Policy = .{
        .kind = implementation.policy_quality,
        .clean_every = 2,
        .turns_since_clean = 1,
    };
    const quality_decision = implementation.decide(&quality, implementation.event_turn);
    try std.testing.expectEqual(implementation.refresh_quality, quality_decision.hint);
    try std.testing.expect(quality_decision.full_update);
    try std.testing.expectEqual(@as(u16, 1), quality.turns_since_clean);
}

test "an undefined kind byte falls into the fast/clean cadence" {
    var policy: implementation.Policy = .{
        .kind = 200,
        .clean_every = 1,
        .turns_since_clean = 0,
    };
    const decision = implementation.decide(&policy, implementation.event_turn);
    try std.testing.expectEqual(implementation.refresh_quality, decision.hint);
    try std.testing.expect(decision.full_update);
}

test "the fast/clean cadence runs its documented sequence" {
    var policy: implementation.Policy = .{
        .kind = implementation.policy_fast_clean,
        .clean_every = 4,
        .turns_since_clean = 0,
    };
    var index: usize = 0;
    while (index < 3) : (index += 1) {
        const decision = implementation.decide(&policy, implementation.event_turn);
        try std.testing.expectEqual(implementation.refresh_fast, decision.hint);
        try std.testing.expect(!decision.full_update);
    }
    try std.testing.expectEqual(@as(u16, 3), policy.turns_since_clean);
    const clean = implementation.decide(&policy, implementation.event_turn);
    try std.testing.expectEqual(implementation.refresh_quality, clean.hint);
    try std.testing.expect(clean.full_update);
    try std.testing.expectEqual(@as(u16, 0), policy.turns_since_clean);
}

test "full-rect dimensions reject either zero side" {
    try std.testing.expect(implementation.fullRectDimensionsValid(1, 1));
    try std.testing.expect(!implementation.fullRectDimensionsValid(0, 1));
    try std.testing.expect(!implementation.fullRectDimensionsValid(1, 0));
    try std.testing.expect(!implementation.fullRectDimensionsValid(0, 0));
}

test "the rectangle built from capabilities covers the whole panel" {
    const rect = implementation.rectFromCaps(.{ .width_px = 1024, .height_px = 600 });
    try std.testing.expectEqual(@as(u16, 0), rect.x);
    try std.testing.expectEqual(@as(u16, 0), rect.y);
    try std.testing.expectEqual(@as(u16, 1024), rect.w);
    try std.testing.expectEqual(@as(u16, 600), rect.h);
    try std.testing.expectEqual(@as(u16, 0), implementation.Rect.empty.w);
}
