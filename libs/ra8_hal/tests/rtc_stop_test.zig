//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rtc_stop.zig (RA8FW-853).

const std = @import("std");
const s = @import("rtc_stop");

/// Real condition unless `stuck`; counts polls.
const Hw = struct {
    polls: *u32,
    stuck: bool = false,
    pub fn eval(self: Hw, _: *volatile u8, _: u32, cond: bool) bool {
        self.polls.* += 1;
        return cond and !self.stuck;
    }
};

test "enterStop clears only START and stops polling once it reads back" {
    var polls: u32 = 0;
    var r: u8 = 0x41;
    s.enterStop(Hw{ .polls = &polls }, &r);
    try std.testing.expectEqual(@as(u8, 0x40), r);
    try std.testing.expectEqual(@as(u32, 1), polls);
}

test "exitStop sets START and keeps the other bits" {
    var polls: u32 = 0;
    var r: u8 = 0x40;
    s.exitStop(Hw{ .polls = &polls }, &r);
    try std.testing.expectEqual(@as(u8, 0x41), r);
    try std.testing.expectEqual(@as(u32, 1), polls);
}

test "waitBit gives up after wait_iters polls" {
    var polls: u32 = 0;
    var r: u8 = 0x00;
    s.waitBit(Hw{ .polls = &polls, .stuck = true }, &r, 0x80, 0x80);
    try std.testing.expectEqual(s.wait_iters, polls);
}
