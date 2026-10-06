//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const gfx = @import("lpm_graphics");
const prcr = gfx.prcr_mod;

const Fake = struct {
    words: [gfx.window_len / 4 + 1]u32 align(4) = @splat(0),

    fn block(f: *Fake) gfx.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }
};

test "an already powered domain is a no-op" {
    var f = Fake{};
    const b = f.block();
    b.mococr().* = 0x01;
    try std.testing.expectEqual(gfx.Outcome.already_on, try gfx.powerOn(b, 10));
    try std.testing.expectEqual(@as(u8, 0x01), b.mococr().*);
    try std.testing.expectEqual(@as(u16, 0), b.prcrReg().*);
}

test "a gated domain starts MOCO, clears PDDE and re-locks PRCR" {
    var f = Fake{};
    const b = f.block();
    b.mococr().* = 0x01;
    b.pdctrgd().* = 0x81;
    try std.testing.expectEqual(gfx.Outcome.powered_on, try gfx.powerOn(b, 10));
    try std.testing.expectEqual(@as(u8, 0x00), b.mococr().*);
    try std.testing.expectEqual(@as(u8, 0x00), b.pdctrgd().*);
    try std.testing.expectEqual(@as(u16, 0xA500), b.prcrReg().*);
}

test "PDCSF stuck busy before power-on times out and leaves PDDE set" {
    var f = Fake{};
    const b = f.block();
    b.pdctrgd().* = 0xC1;
    try std.testing.expectError(error.BusyBeforeOn, gfx.powerOn(b, 5));
    try std.testing.expectEqual(@as(u8, 0xC1), b.pdctrgd().*);
}

test "zero timeout is rejected before any register is touched" {
    var f = Fake{};
    const b = f.block();
    b.pdctrgd().* = 0x81;
    b.mococr().* = 0x01;
    try std.testing.expectError(error.ZeroTimeout, gfx.powerOn(b, 0));
    try std.testing.expectEqual(@as(u8, 0x01), b.mococr().*);
}

test "waitFlag polls at most limit times and the PRCR unlock values match the C" {
    var f = Fake{};
    const b = f.block();
    b.pdctrgd().* = 0x40;
    try std.testing.expect(!gfx.waitFlag(b, gfx.pdcsf_mask, false, 3));
    try std.testing.expect(gfx.waitFlag(b, gfx.pdcsf_mask, true, 1));
    try std.testing.expectEqual(@as(u16, 0xA501), prcr.unlock_cgc);
    try std.testing.expectEqual(@as(u16, 0xA502), prcr.unlock_lpm);
    try std.testing.expectEqual(@as(usize, 0x3FA), gfx.off_prcr);
}
