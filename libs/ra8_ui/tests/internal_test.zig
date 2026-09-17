//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure interaction core: rectangle containment,
//! hit-test ordering, the screen stack, and the page cursor. The C suite's
//! three MC/DC vector sets are mirrored here against the same arithmetic.

const std = @import("std");
const testing = std.testing;
const ui = @import("implementation");

const fixture_rect = ui.Rect{ .x = 10, .y = 20, .w = 30, .h = 40 };

test "rect: top-left corner is inside" {
    try testing.expect(fixture_rect.contains(10, 20));
}

test "rect: right edge is outside" {
    try testing.expect(!fixture_rect.contains(40, 20));
}

test "rect: bottom edge is outside" {
    try testing.expect(!fixture_rect.contains(10, 60));
}

test "rect: last contained pixel is (x+w-1, y+h-1)" {
    try testing.expect(fixture_rect.contains(39, 59));
}

test "rect: zero width contains nothing" {
    const empty = ui.Rect{ .x = 5, .y = 5, .w = 0, .h = 10 };
    try testing.expect(!empty.contains(5, 5));
}

test "rect: zero height contains nothing" {
    const empty = ui.Rect{ .x = 5, .y = 5, .w = 10, .h = 0 };
    try testing.expect(!empty.contains(5, 5));
}

test "rect: negative origin still works" {
    const negative = ui.Rect{ .x = -10, .y = -10, .w = 5, .h = 5 };
    try testing.expect(negative.contains(-10, -6));
    try testing.expect(!negative.contains(-5, -6));
}

test "rect: a width past INT32_MAX no longer wraps" {
    // The C added two int32_t here, so this rectangle's right edge was
    // undefined. Widening to 64-bit makes it mean "everything to the right".
    const huge = ui.Rect{ .x = 1000, .y = 0, .w = std.math.maxInt(i32), .h = 10 };
    try testing.expect(huge.contains(std.math.maxInt(i32), 5));
    try testing.expect(!huge.contains(999, 5));
}

test "rect MC/DC V1: all four conditions true" {
    try testing.expect(fixture_rect.contains(25, 40));
}

test "rect MC/DC V2: px >= x false" {
    try testing.expect(!fixture_rect.contains(5, 40));
}

test "rect MC/DC V3: px < x+w false" {
    try testing.expect(!fixture_rect.contains(45, 40));
}

test "rect MC/DC V4: py >= y false" {
    try testing.expect(!fixture_rect.contains(25, 10));
}

test "rect MC/DC V5: py < y+h false" {
    try testing.expect(!fixture_rect.contains(25, 70));
}

const two_targets = [_]ui.Target{
    .{ .rect = .{ .x = 0, .y = 0, .w = 50, .h = 50 }, .action_id = 100, .reserved = 0 },
    .{ .rect = .{ .x = 50, .y = 0, .w = 50, .h = 50 }, .action_id = 101, .reserved = 0 },
};

test "hitTest: empty list is always a miss" {
    try testing.expect(ui.hitTest(&.{}, 0, 0) == null);
}

test "hitTest: point in the first target" {
    try testing.expectEqual(@as(?u16, 100), ui.hitTest(&two_targets, 25, 25));
}

test "hitTest: point in the second target" {
    try testing.expectEqual(@as(?u16, 101), ui.hitTest(&two_targets, 60, 25));
}

test "hitTest: point outside every target" {
    try testing.expect(ui.hitTest(&two_targets, 200, 200) == null);
}

test "hitTest: earlier entries win on overlap" {
    const overlapping = [_]ui.Target{
        .{ .rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 }, .action_id = 7, .reserved = 0 },
        .{ .rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 }, .action_id = 8, .reserved = 0 },
    };
    try testing.expectEqual(@as(?u16, 7), ui.hitTest(&overlapping, 1, 1));
}

test "nav: init seats the root at depth one" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try testing.expectEqual(@as(u8, 1), nav.depth);
    try testing.expectEqual(@as(u16, 1), try ui.navTop(&nav));
}

test "nav: push then top reports the new screen" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try ui.navPush(&nav, 2);
    try testing.expectEqual(@as(u8, 2), nav.depth);
    try testing.expectEqual(@as(u16, 2), try ui.navTop(&nav));
}

test "nav: push fills to capacity then refuses" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 0);
    var screen: u16 = 1;
    while (screen < ui.nav_max_depth) : (screen += 1) {
        try ui.navPush(&nav, screen);
    }
    try testing.expectEqual(ui.nav_max_depth, nav.depth);
    try testing.expectError(error.Full, ui.navPush(&nav, 99));
    try testing.expectEqual(ui.nav_max_depth, nav.depth);
}

test "nav: push on an uninitialised stack refuses" {
    var nav = ui.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectError(error.NotInitialised, ui.navPush(&nav, 1));
}

test "nav: pop reveals the screen beneath" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try ui.navPush(&nav, 2);
    try ui.navPush(&nav, 3);
    try testing.expectEqual(@as(u16, 2), try ui.navPop(&nav));
    try testing.expectEqual(@as(u8, 2), nav.depth);
}

test "nav: the root is never popped" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try testing.expectError(error.AtRoot, ui.navPop(&nav));
    try testing.expectEqual(@as(u8, 1), nav.depth);
}

test "nav: replace swaps the top without changing depth" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try ui.navPush(&nav, 2);
    try ui.navReplace(&nav, 3);
    try testing.expectEqual(@as(u8, 2), nav.depth);
    try testing.expectEqual(@as(u16, 3), try ui.navTop(&nav));
    try testing.expectEqual(@as(u16, 1), nav.stack[0]);
}

test "nav: replace on an uninitialised stack refuses" {
    var nav = ui.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectError(error.NotInitialised, ui.navReplace(&nav, 1));
}

test "nav: top on an uninitialised stack refuses" {
    const nav = ui.Nav{ .stack = @splat(0), .depth = 0, .reserved = @splat(0) };
    try testing.expectError(error.NotInitialised, ui.navTop(&nav));
}

test "nav: a full drill-down unwinds in LIFO order" {
    var nav: ui.Nav = undefined;
    ui.navInit(&nav, 1);
    try ui.navPush(&nav, 2);
    try ui.navPush(&nav, 3);
    try testing.expectEqual(@as(u16, 2), try ui.navPop(&nav));
    try testing.expectEqual(@as(u16, 1), try ui.navPop(&nav));
    try testing.expectError(error.AtRoot, ui.navPop(&nav));
}

test "pager: a zero total is refused" {
    var pager: ui.Pager = undefined;
    try testing.expectError(error.EmptyTotal, ui.pagerInit(&pager, 0));
}

test "pager: init starts at page zero" {
    var pager: ui.Pager = undefined;
    try ui.pagerInit(&pager, 5);
    try testing.expectEqual(@as(u16, 0), pager.current);
    try testing.expectEqual(@as(u16, 5), pager.total);
}

test "pager: next advances then clamps at the last page" {
    var pager = ui.Pager{ .current = 0, .total = 2 };
    try testing.expect(ui.pagerNext(&pager));
    try testing.expectEqual(@as(u16, 1), pager.current);
    try testing.expect(!ui.pagerNext(&pager));
    try testing.expectEqual(@as(u16, 1), pager.current);
}

test "pager: prev clamps at page zero" {
    var pager = ui.Pager{ .current = 0, .total = 3 };
    try testing.expect(!ui.pagerPrev(&pager));
    try testing.expectEqual(@as(u16, 0), pager.current);
}

test "pager: prev steps back" {
    var pager = ui.Pager{ .current = 2, .total = 3 };
    try testing.expect(ui.pagerPrev(&pager));
    try testing.expectEqual(@as(u16, 1), pager.current);
}

test "pager: goto clamps past the end" {
    var pager = ui.Pager{ .current = 0, .total = 5 };
    try testing.expect(ui.pagerGoto(&pager, 99));
    try testing.expectEqual(@as(u16, 4), pager.current);
}

test "pager: goto to the current page reports no change" {
    var pager = ui.Pager{ .current = 2, .total = 5 };
    try testing.expect(!ui.pagerGoto(&pager, 2));
    try testing.expectEqual(@as(u16, 2), pager.current);
}

test "pager next MC/DC V1: total>0 and current<total-1" {
    var pager = ui.Pager{ .current = 0, .total = 3 };
    try testing.expect(ui.pagerNext(&pager));
    try testing.expectEqual(@as(u16, 1), pager.current);
}

test "pager next MC/DC V2: total>0 false" {
    var pager = ui.Pager{ .current = 0, .total = 0 };
    try testing.expect(!ui.pagerNext(&pager));
    try testing.expectEqual(@as(u16, 0), pager.current);
}

test "pager next MC/DC V3: current<total-1 false" {
    var pager = ui.Pager{ .current = 0, .total = 1 };
    try testing.expect(!ui.pagerNext(&pager));
    try testing.expectEqual(@as(u16, 0), pager.current);
}

test "pager goto MC/DC V1: total>0 and target>total-1 clamps" {
    var pager = ui.Pager{ .current = 0, .total = 3 };
    try testing.expect(ui.pagerGoto(&pager, 5));
    try testing.expectEqual(@as(u16, 2), pager.current);
}

test "pager goto MC/DC V2: total>0 false keeps the target verbatim" {
    var pager = ui.Pager{ .current = 0, .total = 0 };
    try testing.expect(ui.pagerGoto(&pager, 5));
    try testing.expectEqual(@as(u16, 5), pager.current);
}

test "pager goto MC/DC V3: target>total-1 false keeps the target" {
    var pager = ui.Pager{ .current = 0, .total = 3 };
    try testing.expect(ui.pagerGoto(&pager, 1));
    try testing.expectEqual(@as(u16, 1), pager.current);
}
