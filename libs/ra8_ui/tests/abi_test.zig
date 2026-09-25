//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the exported C ABI: return codes, guard order, and the log lines.
//! The last section replays every case in
//! `tests/graphics/src/test_ra8_ui.c` through the exported symbols, so the
//! membrane is exercised the same way the untouched C suite exercises it.
//!
//! `ra8_log_emit_error` is exported here as a counting sink. The library
//! declares it `extern`, so this is the same link-time substitution the real
//! build performs against `libs/ra8_core/src/ra8_log.c`.

const std = @import("std");
const testing = std.testing;
const abi = @import("abi");

const ok: u16 = 0;
const no_mem: u16 = 0x102;
const invalid_arg: u16 = 0x103;
const invalid_state: u16 = 0x104;
const null_ptr: u16 = 0x504;

var log_calls: usize = 0;
var last_message: [*:0]const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    log_calls += 1;
    last_message = message;
}

fn resetLog() void {
    log_calls = 0;
    last_message = "";
}

fn lastMessageIs(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(last_message), expected);
}

const fixture_rect = abi.Rect{ .x = 10, .y = 20, .w = 30, .h = 40 };

const two_targets = [_]abi.Target{
    .{ .rect = .{ .x = 0, .y = 0, .w = 50, .h = 50 }, .action_id = 100, .reserved = 0 },
    .{ .rect = .{ .x = 50, .y = 0, .w = 50, .h = 50 }, .action_id = 101, .reserved = 0 },
};

test "rect_contains: a null rectangle is a miss and logs nothing" {
    resetLog();
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(null, 0, 0));
    try testing.expectEqual(@as(usize, 0), log_calls);
}

test "rect_contains: containment through the ABI" {
    try testing.expectEqual(@as(u8, 1), abi.ra8_ui_rect_contains(&fixture_rect, 10, 20));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 40, 20));
}

test "hit_test: a null out_action is refused first" {
    resetLog();
    var hit: u8 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_hit_test(&two_targets, 2, 0, 0, null, &hit));
    try testing.expectEqual(@as(usize, 1), log_calls);
    try testing.expect(lastMessageIs("out_action must not be nullptr"));
}

test "hit_test: a null out_hit is refused" {
    resetLog();
    var action: u16 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_hit_test(&two_targets, 2, 0, 0, &action, null));
    try testing.expect(lastMessageIs("out_hit must not be nullptr"));
}

test "hit_test: null targets with a nonzero count is refused" {
    resetLog();
    var action: u16 = 0;
    var hit: u8 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_hit_test(null, 2, 0, 0, &action, &hit));
    try testing.expect(lastMessageIs("targets must not be nullptr when count > 0"));
}

test "hit_test: guard order puts out_action ahead of targets" {
    // Both are null; the C's first RA8_CHECK_NULL_PTR decides the message.
    resetLog();
    var hit: u8 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_hit_test(null, 2, 0, 0, null, &hit));
    try testing.expect(lastMessageIs("out_action must not be nullptr"));
}

test "hit_test: a zero count permits null targets and always misses" {
    resetLog();
    var action: u16 = 0;
    var hit: u8 = 1;
    try testing.expectEqual(ok, abi.ra8_ui_hit_test(null, 0, 0, 0, &action, &hit));
    try testing.expectEqual(@as(u8, 0), hit);
    try testing.expectEqual(@as(usize, 0), log_calls);
}

test "hit_test: a hit publishes the action id" {
    var action: u16 = 0;
    var hit: u8 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_hit_test(&two_targets, 2, 60, 25, &action, &hit));
    try testing.expectEqual(@as(u8, 1), hit);
    try testing.expectEqual(@as(u16, 101), action);
}

test "hit_test: a miss leaves the action untouched" {
    var action: u16 = 4242;
    var hit: u8 = 1;
    try testing.expectEqual(ok, abi.ra8_ui_hit_test(&two_targets, 2, 200, 200, &action, &hit));
    try testing.expectEqual(@as(u8, 0), hit);
    try testing.expectEqual(@as(u16, 4242), action);
}

test "nav_init: a null stack is refused" {
    resetLog();
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_init(null, 0));
    try testing.expect(lastMessageIs("nav must not be nullptr"));
}

test "nav_init: seats the root" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));
    try testing.expectEqual(ok, abi.ra8_ui_nav_top(&nav, &top));
    try testing.expectEqual(@as(u16, 1), top);
}

test "nav_push: null, uninitialised, and full all map to their own codes" {
    resetLog();
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_push(null, 1));

    var uninitialised = abi.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectEqual(invalid_state, abi.ra8_ui_nav_push(&uninitialised, 1));

    var full: abi.Nav = undefined;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&full, 0));
    var screen: u16 = 1;
    while (screen < 8) : (screen += 1) {
        try testing.expectEqual(ok, abi.ra8_ui_nav_push(&full, screen));
    }
    try testing.expectEqual(no_mem, abi.ra8_ui_nav_push(&full, 99));
}

test "nav_pop: null nav, null out, and the root all refuse" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));

    resetLog();
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_pop(null, &top));
    try testing.expect(lastMessageIs("nav must not be nullptr"));
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_pop(&nav, null));
    try testing.expect(lastMessageIs("out_screen must not be nullptr"));
    try testing.expectEqual(invalid_state, abi.ra8_ui_nav_pop(&nav, &top));
}

test "nav_pop: reports the revealed screen" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));
    try testing.expectEqual(ok, abi.ra8_ui_nav_push(&nav, 2));
    try testing.expectEqual(ok, abi.ra8_ui_nav_pop(&nav, &top));
    try testing.expectEqual(@as(u16, 1), top);
}

test "nav_replace: null and uninitialised refuse, otherwise the top swaps" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_replace(null, 1));

    var uninitialised = abi.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectEqual(invalid_state, abi.ra8_ui_nav_replace(&uninitialised, 1));

    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));
    try testing.expectEqual(ok, abi.ra8_ui_nav_replace(&nav, 3));
    try testing.expectEqual(ok, abi.ra8_ui_nav_top(&nav, &top));
    try testing.expectEqual(@as(u16, 3), top);
    try testing.expectEqual(@as(u8, 1), nav.depth);
}

test "nav_top: null nav, null out, and uninitialised all refuse" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_top(null, &top));
    try testing.expectEqual(null_ptr, abi.ra8_ui_nav_top(&nav, null));

    const uninitialised = abi.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectEqual(invalid_state, abi.ra8_ui_nav_top(&uninitialised, &top));
}

test "pager_init: null, zero total, then success" {
    var pager: abi.Pager = undefined;
    resetLog();
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_init(null, 5));
    try testing.expect(lastMessageIs("p must not be nullptr"));
    try testing.expectEqual(invalid_arg, abi.ra8_ui_pager_init(&pager, 0));
    try testing.expectEqual(ok, abi.ra8_ui_pager_init(&pager, 5));
    try testing.expectEqual(@as(u16, 0), pager.current);
}

test "pager_next: both pointer guards, then the advance" {
    var pager = abi.Pager{ .current = 0, .total = 2 };
    var changed: u8 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_next(null, &changed));
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_next(&pager, null));
    try testing.expectEqual(ok, abi.ra8_ui_pager_next(&pager, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(ok, abi.ra8_ui_pager_next(&pager, &changed));
    try testing.expectEqual(@as(u8, 0), changed);
}

test "pager_prev: both pointer guards, then the clamp" {
    var pager = abi.Pager{ .current = 0, .total = 3 };
    var changed: u8 = 1;
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_prev(null, &changed));
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_prev(&pager, null));
    try testing.expectEqual(ok, abi.ra8_ui_pager_prev(&pager, &changed));
    try testing.expectEqual(@as(u8, 0), changed);
    try testing.expectEqual(@as(u16, 0), pager.current);
}

test "pager_goto: both pointer guards, then the clamp past the end" {
    var pager = abi.Pager{ .current = 0, .total = 5 };
    var changed: u8 = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_goto(null, 1, &changed));
    try testing.expectEqual(null_ptr, abi.ra8_ui_pager_goto(&pager, 1, null));
    try testing.expectEqual(ok, abi.ra8_ui_pager_goto(&pager, 99, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 4), pager.current);
}

test "C suite mirror: rect_contains edges" {
    try testing.expectEqual(@as(u8, 1), abi.ra8_ui_rect_contains(&fixture_rect, 10, 20));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 40, 20));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 10, 60));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(null, 0, 0));
}

test "C suite mirror: rect_contains MC/DC vectors" {
    try testing.expectEqual(@as(u8, 1), abi.ra8_ui_rect_contains(&fixture_rect, 25, 40));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 5, 40));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 45, 40));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 25, 10));
    try testing.expectEqual(@as(u8, 0), abi.ra8_ui_rect_contains(&fixture_rect, 25, 70));
}

test "C suite mirror: nav stack lifecycle" {
    var nav: abi.Nav = undefined;
    var top: u16 = 0;
    try testing.expectEqual(ok, abi.ra8_ui_nav_init(&nav, 1));
    try testing.expectEqual(ok, abi.ra8_ui_nav_top(&nav, &top));
    try testing.expectEqual(@as(u16, 1), top);
    try testing.expectEqual(ok, abi.ra8_ui_nav_push(&nav, 2));
    try testing.expectEqual(ok, abi.ra8_ui_nav_push(&nav, 3));
    try testing.expectEqual(ok, abi.ra8_ui_nav_top(&nav, &top));
    try testing.expectEqual(@as(u16, 3), top);
    try testing.expectEqual(ok, abi.ra8_ui_nav_pop(&nav, &top));
    try testing.expectEqual(@as(u16, 2), top);
    try testing.expectEqual(ok, abi.ra8_ui_nav_replace(&nav, 3));
    try testing.expectEqual(ok, abi.ra8_ui_nav_top(&nav, &top));
    try testing.expectEqual(@as(u16, 3), top);
    try testing.expectEqual(ok, abi.ra8_ui_nav_pop(&nav, &top));
    try testing.expectEqual(@as(u16, 1), top);
    try testing.expectEqual(invalid_state, abi.ra8_ui_nav_pop(&nav, &top));
}

test "C suite mirror: pager basic" {
    var pager: abi.Pager = undefined;
    var changed: u8 = 0;
    try testing.expectEqual(invalid_arg, abi.ra8_ui_pager_init(&pager, 0));
    try testing.expectEqual(ok, abi.ra8_ui_pager_init(&pager, 5));
    try testing.expectEqual(@as(u16, 0), pager.current);
    try testing.expectEqual(ok, abi.ra8_ui_pager_prev(&pager, &changed));
    try testing.expectEqual(@as(u8, 0), changed);
    try testing.expectEqual(ok, abi.ra8_ui_pager_goto(&pager, 99, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 4), pager.current);
    try testing.expectEqual(ok, abi.ra8_ui_pager_prev(&pager, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 3), pager.current);
}

test "C suite mirror: pager_next MC/DC vectors" {
    var changed: u8 = 0;
    var v1 = abi.Pager{ .current = 0, .total = 3 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_next(&v1, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 1), v1.current);

    var v2 = abi.Pager{ .current = 0, .total = 0 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_next(&v2, &changed));
    try testing.expectEqual(@as(u8, 0), changed);

    var v3 = abi.Pager{ .current = 0, .total = 1 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_next(&v3, &changed));
    try testing.expectEqual(@as(u8, 0), changed);
}

test "C suite mirror: pager_goto MC/DC vectors" {
    var changed: u8 = 0;
    var v1 = abi.Pager{ .current = 0, .total = 3 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_goto(&v1, 5, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 2), v1.current);

    var v2 = abi.Pager{ .current = 0, .total = 0 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_goto(&v2, 5, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 5), v2.current);

    var v3 = abi.Pager{ .current = 0, .total = 3 };
    try testing.expectEqual(ok, abi.ra8_ui_pager_goto(&v3, 1, &changed));
    try testing.expectEqual(@as(u8, 1), changed);
    try testing.expectEqual(@as(u16, 1), v3.current);
}
