//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_life.zig.

const std = @import("std");
const life = @import("dotf_life");

const Log = struct {
    buf: [64]u8 = undefined,
    len: usize = 0,
    fail_id: u16 = 0,
    fail_err: u16 = 0,
    logged: u16 = 0,

    fn put(self: *Log, c: u8) void {
        self.buf[self.len] = c;
        self.len += 1;
    }
    fn text(self: *const Log) []const u8 {
        return self.buf[0..self.len];
    }
    // e<id low bit> = mstp enable, d = mstp disable, r/s/x + channel digit,
    // h = handler cleared, i = info, f = fail.
    pub fn mstpEnable(self: *Log, id: u16) u16 {
        self.put('e');
        self.put('0' + @as(u8, @intCast(id & 1)));
        return if (id == self.fail_id) self.fail_err else 0;
    }
    pub fn mstpDisable(self: *Log, id: u16) u16 {
        self.put('d');
        self.put('0' + @as(u8, @intCast(id & 1)));
        return 0x203;
    }
    pub fn channelReset(self: *Log, ch: u8) void {
        self.put('r');
        self.put('0' + ch);
    }
    pub fn disable(self: *Log, ch: u8) void {
        self.put('x');
        self.put('0' + ch);
    }
    pub fn stateReset(self: *Log, ch: u8) void {
        self.put('s');
        self.put('0' + ch);
    }
    pub fn clearHandler(self: *Log) void {
        self.put('h');
    }
    pub fn fail(self: *Log, _: [*:0]const u8, err: u16) void {
        self.put('f');
        self.logged = err;
    }
    pub fn info(self: *Log, _: [*:0]const u8) void {
        self.put('i');
    }
};

test "init gates, resets each channel in order, then clears the handler" {
    var log = Log{};
    try std.testing.expectEqual(@as(u16, 0), life.init(&log));
    try std.testing.expectEqualStrings("e0r0s0e1r1s1hi", log.text());
}

test "init stops at the first MSTP failure without clearing the handler" {
    var log = Log{ .fail_id = (1 << 8) | 17, .fail_err = 0x104 };
    try std.testing.expectEqual(@as(u16, 0x104), life.init(&log));
    try std.testing.expectEqualStrings("e0r0s0e1f", log.text());
    try std.testing.expectEqual(@as(u16, 0x104), log.logged);
}

test "deinit bypasses, resets and ungates every channel despite MSTP errors" {
    var log = Log{};
    life.deinit(&log);
    try std.testing.expectEqualStrings("x0s0d0x1s1d1h", log.text());
}
