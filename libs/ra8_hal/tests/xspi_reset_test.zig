//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_reset.zig (RA8FW-867).

const std = @import("std");
const rst = @import("xspi_reset");

const Fake = struct {
    cdts: [4]u32 = undefined,
    n: usize = 0,
    zeroed: usize = 0,
    fail_on: ?usize = null,

    pub fn write(self: *Fake, off: usize, v: u32) void {
        if (off == rst.off_cdt) {
            self.cdts[self.n] = v;
            self.n += 1;
        } else if (v == 0) {
            self.zeroed += 1;
        }
    }
    pub fn kick(self: *Fake) u16 {
        if (self.fail_on) |k| if (k + 1 == self.n) return 0x203;
        return 0;
    }
};

test "cdtWord left-justifies 1S and pairs the complement for 8D" {
    try std.testing.expectEqual(@as(u32, 0x6600_8001), rst.cdtWord(0x66, 1));
    try std.testing.expectEqual(@as(u32, 0x9966_8002), rst.cdtWord(0x66, 2));
    try std.testing.expectEqual(@as(u32, 0x6699_8002), rst.cdtWord(0x99, 2));
}

test "reset issues RSTEN then RST and zeroes the rest of the slot" {
    var f = Fake{};
    try std.testing.expectEqual(@as(u16, 0), try rst.reset(&f, 1));
    try std.testing.expectEqual(@as(usize, 2), f.n);
    try std.testing.expectEqual(rst.cdtWord(0x66, 1), f.cdts[0]);
    try std.testing.expectEqual(rst.cdtWord(0x99, 1), f.cdts[1]);
    try std.testing.expectEqual(@as(usize, 6), f.zeroed);
}

test "reset stops at the first failing kick" {
    var f = Fake{ .fail_on = 0 };
    try std.testing.expectEqual(@as(u16, 0x203), try rst.reset(&f, 2));
    try std.testing.expectEqual(@as(usize, 1), f.n);
}

test "reset rejects command widths other than 1 and 2" {
    var f = Fake{};
    try std.testing.expectError(error.InvalidArg, rst.reset(&f, 0));
    try std.testing.expectError(error.InvalidArg, rst.reset(&f, 3));
    try std.testing.expectEqual(@as(usize, 0), f.n);
}
