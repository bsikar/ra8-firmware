//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure paint geometry: pen placement and proportional fill.

const std = @import("std");
const paint = @import("implementation");

const rect: paint.Rect = .{ .x = 10, .y = 20, .w = 100, .h = 40 };

test "inset pen hugs the rect's top-left plus the pad" {
    const pen = paint.insetPen(rect, 3);
    try std.testing.expectEqual(@as(i32, 13), pen.x);
    try std.testing.expectEqual(@as(i32, 23), pen.y);
}

test "inset pen with no pad is the rect origin" {
    const pen = paint.insetPen(rect, 0);
    try std.testing.expectEqual(@as(i32, 10), pen.x);
    try std.testing.expectEqual(@as(i32, 20), pen.y);
}

test "centered x splits the leftover width evenly" {
    try std.testing.expectEqual(@as(i32, 10 + (100 - 16) / 2), paint.centeredX(rect, 16));
}

test "centered x of a string wider than the rect leans left of the origin" {
    try std.testing.expectEqual(@as(i32, 10 + (100 - 140) / 2), paint.centeredX(rect, 140));
}

test "right x hugs the right inner inset" {
    try std.testing.expectEqual(@as(i32, (10 + 100) - 3 - 16), paint.rightX(rect, 3, 16));
}

test "centered y centres the glyph cell vertically" {
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), paint.centeredY(rect, 12));
}

test "measured pen keeps the inset on the x axis for left alignment" {
    const pen = paint.measuredPen(rect, 3, .left, 16, 12);
    try std.testing.expectEqual(@as(i32, 13), pen.x);
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), pen.y);
}

test "measured pen centres both axes for centre alignment" {
    const pen = paint.measuredPen(rect, 3, .center, 16, 12);
    try std.testing.expectEqual(@as(i32, 10 + (100 - 16) / 2), pen.x);
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), pen.y);
}

test "measured pen hugs the right inset for right alignment" {
    const pen = paint.measuredPen(rect, 3, .right, 16, 12);
    try std.testing.expectEqual(@as(i32, (10 + 100) - 3 - 16), pen.x);
    try std.testing.expectEqual(@as(i32, 20 + (40 - 12) / 2), pen.y);
}

test "fill frac of a zero total is empty" {
    try std.testing.expectEqual(@as(i32, 0), paint.fillFrac(5, 0, 100));
}

test "fill frac of a non-positive width is empty" {
    try std.testing.expectEqual(@as(i32, 0), paint.fillFrac(5, 10, 0));
    try std.testing.expectEqual(@as(i32, 0), paint.fillFrac(5, 10, -4));
}

test "fill frac of a zero value is empty" {
    try std.testing.expectEqual(@as(i32, 0), paint.fillFrac(0, 10, 100));
}

test "fill frac is proportional and truncates toward zero" {
    try std.testing.expectEqual(@as(i32, 50), paint.fillFrac(5, 10, 100));
    try std.testing.expectEqual(@as(i32, 33), paint.fillFrac(1, 3, 100));
}

test "fill frac clamps a value at or above the total to the whole width" {
    try std.testing.expectEqual(@as(i32, 100), paint.fillFrac(10, 10, 100));
    try std.testing.expectEqual(@as(i32, 100), paint.fillFrac(99, 10, 100));
}

test "fill frac never exceeds the width for any value" {
    var value: u32 = 0;
    while (value <= 40) : (value += 1) {
        const filled = paint.fillFrac(value, 20, 64);
        try std.testing.expect(filled >= 0);
        try std.testing.expect(filled <= 64);
    }
}
