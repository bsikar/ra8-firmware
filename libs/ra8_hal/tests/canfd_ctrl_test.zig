//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_ctrl.zig (RA8FW-861).

const std = @import("std");
const ctrl = @import("canfd_ctrl");

const Fake = struct {
    ctr: u32 = 0,
    halt_err: u16 = 0,
    trail: [8]u8 = undefined,
    n: usize = 0,

    pub fn setMode(self: *Fake, mode: ctrl.Mode) u16 {
        self.trail[self.n] = switch (mode) {
            .operation => 'o',
            .reset => 'r',
            .halt => 'h',
        };
        self.n += 1;
        return if (mode == .halt) self.halt_err else 0;
    }
    pub fn readCtr(self: *Fake) u32 {
        return self.ctr;
    }
    pub fn writeCtr(self: *Fake, v: u32) void {
        self.trail[self.n] = 'w';
        self.n += 1;
        self.ctr = v;
    }
};

test "testModeCtr replaces CTMS and sets CTME, keeping other bits" {
    try std.testing.expectEqual(@as(u32, 0x0700_0005), ctrl.testModeCtr(0x0300_0005, 3));
    try std.testing.expectEqual(@as(u32, 0x0300_0000), ctrl.testModeCtr(0x0600_0000, 1));
    try std.testing.expectEqual(@as(u32, 0x0100_0000), ctrl.testModeCtr(0, 0));
}

test "setTestMode halts, writes CTR, returns to operation" {
    var f = Fake{ .ctr = 0x4 };
    try std.testing.expectEqual(@as(u16, 0), try ctrl.setTestMode(&f, 2));
    try std.testing.expectEqualStrings("hwo", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u32, 0x0500_0004), f.ctr);
}

test "setTestMode reports a halt failure after recovering to operation" {
    var f = Fake{ .halt_err = 0x203 };
    try std.testing.expectEqual(@as(u16, 0x203), try ctrl.setTestMode(&f, 1));
    try std.testing.expectEqualStrings("ho", f.trail[0..f.n]);
}

test "setTestMode rejects modes past self-test 1 without touching hardware" {
    var f = Fake{};
    try std.testing.expectError(error.InvalidMode, ctrl.setTestMode(&f, 4));
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "isoValue toggles NISO only" {
    try std.testing.expectEqual(@as(u32, 0xF1), ctrl.isoValue(0xF0, true));
    try std.testing.expectEqual(@as(u32, 0xF0), ctrl.isoValue(0xF1, false));
}
