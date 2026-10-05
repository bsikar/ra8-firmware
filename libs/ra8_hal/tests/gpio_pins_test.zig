//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GPIO pin-level ops (RA8FW-763).

const std = @import("std");
const gp = @import("gpio_pins");

const Rec = struct {
    addrs: [8]usize = undefined,
    vals: [8]u8 = undefined,
    n: usize = 0,
    pub fn write8(self: *Rec, addr: usize, value: u8) void {
        self.addrs[self.n] = addr;
        self.vals[self.n] = value;
        self.n += 1;
    }
};

fn pinOf(v: u16) gp.Pin {
    return switch (gp.decode(v)) {
        .pin => |p| p,
        .err => unreachable,
    };
}

test "decode splits port and pin and checks both ranges" {
    const p = pinOf(0x0E0F);
    try std.testing.expectEqual(@as(u8, 14), p.port);
    try std.testing.expectEqual(@as(u8, 15), p.bit);
    try std.testing.expectEqual(gp.Status.invalid_port, gp.decode(0x0F00).err);
    try std.testing.expectEqual(gp.Status.invalid_pin, gp.decode(0x0010).err);
}

test "port and PFS addresses" {
    try std.testing.expectEqual(@as(usize, 0x4040_00A8), gp.portReg(5, gp.off_pcntr3));
    try std.testing.expectEqual(@as(usize, 0x4040_0800), gp.pfsAddr(pinOf(0x0000)));
    try std.testing.expectEqual(@as(usize, 0x4040_0B38), gp.pfsAddr(pinOf(0x0C0E)));
}

test "write uses POSR to set and PORR to clear" {
    const p = pinOf(0x0103);
    try std.testing.expectEqual(@as(u32, 0x0000_0008), gp.writeValue(p, true));
    try std.testing.expectEqual(@as(u32, 0x0008_0000), gp.writeValue(p, false));
}

test "toggle flips from PODR and read takes PIDR" {
    const p = pinOf(0x0002);
    try std.testing.expectEqual(@as(u32, 0x0004_0000), gp.toggleValue(p, 0x0004_0000));
    try std.testing.expectEqual(@as(u32, 0x0000_0004), gp.toggleValue(p, 0x0000_0004));
    try std.testing.expect(gp.levelOf(p, 0x0000_0004));
    try std.testing.expect(!gp.levelOf(p, 0x0004_0000));
}

test "drive strength only changes bits 11:10" {
    try std.testing.expectEqual(@as(u32, 0x0001_0C05), gp.withDscr(0x0001_0005, 3));
    try std.testing.expectEqual(@as(u32, 0x0001_0405), gp.withDscr(0x0001_0805, 1));
    try std.testing.expectEqual(@as(u32, 0x0000_0000), gp.withDscr(0x0000_0C00, 4));
}

test "unlock then lock write PWPR and PWPRS in order" {
    var r = Rec{};
    gp.unlock(&r);
    gp.lock(&r);
    const want_a = [_]usize{ 0x4040_0D0C, 0x4040_0D0C, 0x4040_0D14, 0x4040_0D14 } ** 2;
    const want_v = [_]u8{ 0, 0x40, 0, 0x40, 0, 0x80, 0, 0x80 };
    try std.testing.expectEqualSlices(usize, &want_a, r.addrs[0..r.n]);
    try std.testing.expectEqualSlices(u8, &want_v, r.vals[0..r.n]);
}
