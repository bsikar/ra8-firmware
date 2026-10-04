//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const cac = @import("cac");

/// Host RAM standing in for the CAC register file (12 bytes).
const Fake = struct {
    bytes: [12]u8 align(2) = [_]u8{0} ** 12,

    fn block(f: *Fake) cac.Block {
        return .{ .base = @intFromPtr(&f.bytes) };
    }

    fn u16At(f: *Fake, off: usize) u16 {
        return std.mem.readInt(u16, f.bytes[off..][0..2], .little);
    }
};

test "configure stops measuring, clears every flag and loads the limits" {
    var f = Fake{};
    f.bytes[cac.off_cacr0] = cac.cfme;
    f.bytes[cac.off_cacr1] = 0xFF;
    f.bytes[cac.off_cacr2] = 0xFF;
    f.block().configure(0x1234, 0x0567);
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr0]);
    try std.testing.expectEqual(@as(u8, 0x70), f.bytes[cac.off_caicr]);
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr1]);
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr2]);
    try std.testing.expectEqual(@as(u16, 0x1234), f.u16At(cac.off_caulvr));
    try std.testing.expectEqual(@as(u16, 0x0567), f.u16At(cac.off_callvr));
}

test "measure returns CACNTBR once MENDF is set and leaves CFME clear" {
    var f = Fake{};
    f.bytes[cac.off_castr] = cac.status_mendf;
    std.mem.writeInt(u16, f.bytes[cac.off_cacntbr..][0..2], 0xBEEF, .little);
    try std.testing.expectEqual(@as(u16, 0xBEEF), try f.block().measure());
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr0]);
}

test "measure times out without MENDF and still clears CFME" {
    var f = Fake{};
    f.bytes[cac.off_castr] = cac.status_ferrf | cac.status_ovff;
    try std.testing.expectError(error.Timeout, f.block().measure());
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr0]);
}

test "clear maps CASTR bits to their CAICR clear bits and drops the rest" {
    try std.testing.expectEqual(@as(u8, 0x20), cac.Block.clearBits(cac.status_mendf));
    try std.testing.expectEqual(@as(u8, 0x70), cac.Block.clearBits(0xFF));
    var f = Fake{};
    f.block().clear(cac.status_ferrf | 0x80);
    try std.testing.expectEqual(@as(u8, 0x10), f.bytes[cac.off_caicr]);
}

test "takePending and shutdown acknowledge flags and zero the control bytes" {
    var f = Fake{};
    f.bytes[cac.off_castr] = 0xF0 | cac.status_ovff | cac.status_mendf;
    const b = f.block();
    try std.testing.expectEqual(@as(u8, 0x06), b.status());
    try std.testing.expectEqual(@as(u8, 0x06), b.takePending());
    try std.testing.expectEqual(@as(u8, 0x60), f.bytes[cac.off_caicr]);
    f.bytes[cac.off_cacr0] = cac.cfme;
    f.bytes[cac.off_cacr1] = 3;
    b.shutdown();
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr0]);
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_cacr1]);
    try std.testing.expectEqual(@as(u8, 0), f.bytes[cac.off_caicr]);
}
