//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const sec = @import("bkup_security");
const prcr = sec.prcr_mod;

const Fake = struct {
    words: [sec.window_len / 4 + 1]u32 align(4) = [_]u32{0} ** (sec.window_len / 4 + 1),

    fn block(f: *Fake) sec.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }
};

const good = sec.Config{ .bbfsar = 0x55, .saba = 0x0100, .pabas = 0x0200, .pabans = 0xFFE0 };

test "apply writes all four registers and re-locks PRCR" {
    var f = Fake{};
    const b = f.block();
    try sec.apply(b, good);
    try std.testing.expectEqual(@as(u32, 0x55), b.ra8_bkup_bbfsar().*);
    try std.testing.expectEqual(@as(u16, 0x0100), b.ra8_bkup_vbrsabar().*);
    try std.testing.expectEqual(@as(u16, 0x0200), b.ra8_bkup_vbrpabars().*);
    try std.testing.expectEqual(@as(u16, 0xFFE0), b.ra8_bkup_vbrpabarns().*);
    try std.testing.expectEqual(@as(u16, 0xA500), b.prcrReg().*);
}

test "apply rejects BBFSAR bits outside the NONSEC mask without touching anything" {
    var f = Fake{};
    const b = f.block();
    var cfg = good;
    cfg.bbfsar = 0x80;
    try std.testing.expectError(error.BadBbfsar, sec.apply(b, cfg));
    try std.testing.expectEqual(@as(u32, 0), b.ra8_bkup_bbfsar().*);
    try std.testing.expectEqual(@as(u16, 0), b.prcrReg().*);
}

test "apply names the first misaligned boundary" {
    var f = Fake{};
    const b = f.block();
    var cfg = good;
    cfg.pabans = 0x0101;
    try std.testing.expectError(error.BadPabans, sec.apply(b, cfg));
    cfg.pabas = 0x0010;
    try std.testing.expectError(error.BadPabas, sec.apply(b, cfg));
    cfg.saba = 0x001F;
    try std.testing.expectError(error.BadSaba, sec.apply(b, cfg));
    try std.testing.expectEqual(@as(u16, 0), b.ra8_bkup_vbrsabar().*);
}

test "get reads back with BBFSAR masked" {
    var f = Fake{};
    const b = f.block();
    b.ra8_bkup_bbfsar().* = 0xFFFF_FFFF;
    b.ra8_bkup_vbrsabar().* = 0x0040;
    b.ra8_bkup_vbrpabars().* = 0x0060;
    b.ra8_bkup_vbrpabarns().* = 0x0080;
    const got = sec.get(b);
    try std.testing.expectEqual(@as(u32, 0x7F), got.bbfsar);
    try std.testing.expectEqual(@as(u16, 0x0040), got.saba);
    try std.testing.expectEqual(@as(u16, 0x0060), got.pabas);
    try std.testing.expectEqual(@as(u16, 0x0080), got.pabans);
}

test "prcr window writes the unlock value then the lock-all password" {
    var reg: u16 = 0;
    const window = prcr.open(&reg, prcr.unlock_sar);
    try std.testing.expectEqual(@as(u16, 0xA510), reg);
    window.close();
    try std.testing.expectEqual(@as(u16, 0xA500), reg);
    try std.testing.expectEqual(@as(usize, 0x3FA), sec.off_prcr);
}
