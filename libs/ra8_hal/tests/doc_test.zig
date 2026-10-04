//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const doc = @import("doc");

/// Host RAM standing in for the DOC_B register file (no arithmetic engine).
const Fake = struct {
    words: [6]u32 align(4) = [_]u32{0} ** 6,

    fn block(f: *Fake) doc.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }

    fn at(f: *Fake, off: usize) u32 {
        return f.words[off / 4];
    }
};

test "reset selects compare/16-bit, clears DOPCF and zeroes the data registers" {
    var f = Fake{ .words = .{ 0xFF, 0, 0, 0xFFFF_FFFF, 0xFFFF_FFFF, 0xFFFF_FFFF } };
    f.block().reset();
    try std.testing.expectEqual(@as(u32, 0), f.at(doc.off_docr));
    try std.testing.expectEqual(@as(u32, doc.mask_dopcfcl), f.at(doc.off_doscr));
    try std.testing.expectEqual(@as(u32, 0), f.at(doc.off_dodir));
    try std.testing.expectEqual(@as(u32, 0), f.at(doc.off_dodsr0));
    try std.testing.expectEqual(@as(u32, 0), f.at(doc.off_dodsr1));
}

test "add16 and sub16 select the mode, seed DODSR0 and trigger with DODIR" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expectEqual(@as(u16, 0x1234), b.add16(0x1234, 0x0F0F));
    try std.testing.expectEqual(@as(u32, doc.mode_add), f.at(doc.off_docr));
    try std.testing.expectEqual(@as(u32, 0x0F0F), f.at(doc.off_dodir));
    _ = b.sub16(7, 3);
    try std.testing.expectEqual(@as(u32, doc.mode_subtract), f.at(doc.off_docr));
    try std.testing.expectEqual(@as(u32, 7), f.at(doc.off_dodsr0));
}

test "setWindow programs DCSEL and thresholds and rejects bad input" {
    var f = Fake{};
    const b = f.block();
    try b.setWindow(10, 20, doc.window_inside);
    try std.testing.expectEqual(@as(u32, 0x40), f.at(doc.off_docr));
    try std.testing.expectEqual(@as(u32, 10), f.at(doc.off_dodsr0));
    try std.testing.expectEqual(@as(u32, 20), f.at(doc.off_dodsr1));
    try std.testing.expectEqual(@as(u32, 1), f.at(doc.off_doscr));
    try b.setWindow(1, 2, doc.window_outside);
    try std.testing.expectEqual(@as(u32, 0x50), f.at(doc.off_docr));
    try std.testing.expectError(error.BadRange, b.setWindow(5, 5, 0));
    try std.testing.expectError(error.BadPolarity, b.setWindow(1, 2, 2));
}

test "windowCompare needs compare mode and reports the staged DOPCF" {
    var f = Fake{};
    const b = f.block();
    f.words[0] = doc.mode_add;
    try std.testing.expectError(error.NotCompareMode, b.windowCompare(1));
    try b.setWindow(10, 20, doc.window_inside);
    f.words[doc.off_dosr / 4] = 1;
    try std.testing.expect(try b.windowCompare(15));
    try std.testing.expectEqual(@as(u32, 15), f.at(doc.off_dodir));
    f.words[doc.off_dosr / 4] = 0;
    try std.testing.expect(!try b.windowCompare(25));
}

test "16-bit data writes leave the upper halfword alone" {
    var f = Fake{ .words = .{ 0, 0, 0, 0xABCD_0000, 0, 0 } };
    _ = f.block().add16(1, 2);
    try std.testing.expectEqual(@as(u32, 0xABCD_0002), f.at(doc.off_dodir));
}
