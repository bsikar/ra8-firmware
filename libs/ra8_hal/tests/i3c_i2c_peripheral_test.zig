//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const p = @import("i3c_i2c_peripheral");

const Fake = struct {
    words: [p.window_len / 4]u32 = [_]u32{0} ** (p.window_len / 4),

    fn block(f: *Fake) p.Block {
        return .{ .base = @intFromPtr(&f.words) };
    }
};

test "regsFor accepts only channel 0" {
    try std.testing.expectEqual(@as(usize, p.base), p.regsFor(0).?.base);
    try std.testing.expect(p.regsFor(1) == null);
}

test "configure and clear program the responder registers" {
    var f = Fake{};
    const b = f.block();
    b.reg(p.off_rstctl).* = 1;
    p.configure(b, .{ .peripheral_addr_7b = 0x42, .general_call = 1 });
    try std.testing.expectEqual(@as(u32, 0), b.reg(p.off_rstctl).*);
    try std.testing.expectEqual(@as(u32, 0x84), b.reg(p.off_msdvad).*);
    try std.testing.expectEqual(p.svctl_gcae, b.reg(p.off_svctl).*);
    try std.testing.expectEqual(p.bctl_buse, b.reg(p.off_bctl).*);
    p.configure(b, .{ .peripheral_addr_7b = 0x10, .general_call = 0 });
    try std.testing.expectEqual(@as(u32, 0), b.reg(p.off_svctl).*);
    p.clear(b);
    try std.testing.expectEqual(@as(u32, 0), b.reg(p.off_bctl).*);
    try std.testing.expectEqual(@as(u32, 0), b.reg(p.off_msdvad).*);
}

test "send writes each byte while TDBEF0 is set and times out otherwise" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expectError(error.Timeout, p.send(b, &.{0x11}, 4));
    b.reg(p.off_ntst).* = p.ntst_tdbef0;
    try p.send(b, &.{ 0x11, 0x22, 0x33 }, 4);
    try std.testing.expectEqual(@as(u32, 0x33), b.reg(p.off_ntdtbp0).*);
}

test "receive masks NTDTBP0 to a byte while RDBFF0 is set and times out otherwise" {
    var f = Fake{};
    const b = f.block();
    var buf = [_]u8{0} ** 3;
    try std.testing.expectError(error.Timeout, p.receive(b, &buf, 4));
    b.reg(p.off_ntst).* = p.ntst_rdbff0;
    b.reg(p.off_ntdtbp0).* = 0x1A5;
    try p.receive(b, &buf, 4);
    try std.testing.expectEqualSlices(u8, &.{ 0xA5, 0xA5, 0xA5 }, &buf);
}

test "statusMask maps NTST, BST and MSDVAD" {
    var f = Fake{};
    const b = f.block();
    try std.testing.expectEqual(@as(u8, 0), p.statusMask(b));
    b.reg(p.off_ntst).* = p.ntst_rdbff0 | p.ntst_tdbef0;
    b.reg(p.off_bst).* = p.bst_spcnddf | p.bst_nackdf;
    b.reg(p.off_msdvad).* = 0x84;
    try std.testing.expectEqual(@as(u8, 0x1F), p.statusMask(b));
}
