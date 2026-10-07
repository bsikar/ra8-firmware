//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rtc_calendar.zig (RA8FW-854).

const std = @import("std");
const c = @import("rtc_calendar");

/// Records each wait's expected START value and the logged year.
const Hw = struct {
    waits: *std.ArrayList(u8),
    year: *u32,
    pub fn wait(self: Hw, _: *volatile u8, _: u8, expect: u8) void {
        self.waits.append(std.testing.allocator, expect) catch unreachable;
    }
    pub fn infoVal(self: Hw, _: [*:0]const u8, value: u32) void {
        self.year.* = value;
    }
};

fn zeroCal() c.Cal {
    return std.mem.zeroes(c.Cal);
}

test "BCD round trips 0..99" {
    var i: u8 = 0;
    while (i < 100) : (i += 1) try std.testing.expectEqual(i, c.bcdToBin(c.binToBcd(i)));
    try std.testing.expectEqual(@as(u8, 0x59), c.binToBcd(59));
}

test "set writes BCD counters with START dropped then restored, and get reads them back" {
    var waits: std.ArrayList(u8) = .empty;
    defer waits.deinit(std.testing.allocator);
    var year: u32 = 0;
    var cal = zeroCal();
    var rcr2: u8 = 0x41;
    const dt = c.Datetime{ .year = 2026, .month = 10, .day = 6, .weekday = 2, .hour = 3, .minute = 45, .second = 9 };
    try std.testing.expectEqual(c.ok, c.set(Hw{ .waits = &waits, .year = &year }, &cal, &rcr2, &dt));
    try std.testing.expectEqualSlices(u8, &.{ 0, 1 }, waits.items);
    try std.testing.expectEqual(@as(u8, 0x41), rcr2);
    try std.testing.expectEqual(@as(u16, 0x26), cal.ryrcnt);
    try std.testing.expectEqual(@as(u8, 0x45), cal.rmincnt);
    try std.testing.expectEqual(@as(u32, 2026), year);
    var out: c.Datetime = undefined;
    c.get(&cal, &out);
    try std.testing.expectEqual(dt, out);
}

test "set rejects a year before 2000 without touching registers" {
    var waits: std.ArrayList(u8) = .empty;
    defer waits.deinit(std.testing.allocator);
    var year: u32 = 0;
    var cal = zeroCal();
    var rcr2: u8 = 0x41;
    const dt = c.Datetime{ .year = 1999, .month = 1, .day = 1, .weekday = 0, .hour = 0, .minute = 0, .second = 0 };
    try std.testing.expectEqual(c.invalid_arg, c.set(Hw{ .waits = &waits, .year = &year }, &cal, &rcr2, &dt));
    try std.testing.expectEqual(@as(usize, 0), waits.items.len);
    try std.testing.expectEqual(@as(u8, 0x41), rcr2);
}

test "setAlarm enables hh:mm:ss, wildcards the date and checks ranges" {
    var cal = zeroCal();
    cal.rwkar = 0xFF;
    cal.ryraren = 0xFF;
    var a = c.Datetime{ .year = 0, .month = 0, .day = 0, .weekday = 0, .hour = 23, .minute = 59, .second = 30 };
    try std.testing.expectEqual(c.ok, c.setAlarm(&cal, &a));
    try std.testing.expectEqual(@as(u8, 0xB0), cal.rsecar);
    try std.testing.expectEqual(@as(u8, 0xD9), cal.rminar);
    try std.testing.expectEqual(@as(u8, 0xA3), cal.rhrar);
    try std.testing.expectEqual(@as(u8, 0), cal.rwkar | cal.ryraren);
    a.hour = 24;
    try std.testing.expectEqual(c.invalid_arg, c.setAlarm(&cal, &a));
    a.hour = 0;
    a.second = 60;
    try std.testing.expectEqual(c.invalid_arg, c.setAlarm(&cal, &a));
}
