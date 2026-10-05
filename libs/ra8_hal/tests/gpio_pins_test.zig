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

const Pfs = struct {
    w8: usize = 0,
    vals: [4]u32 = undefined,
    n32: usize = 0,
    order: [16]u8 = undefined,
    n: usize = 0,
    pub fn write8(self: *Pfs, _: usize, _: u8) void {
        self.w8 += 1;
        self.order[self.n] = 8;
        self.n += 1;
    }
    pub fn write32(self: *Pfs, addr: usize, value: u32) void {
        std.debug.assert(addr == 0x4040_0804);
        self.vals[self.n32] = value;
        self.n32 += 1;
        self.order[self.n] = 32;
        self.n += 1;
    }
};

test "init values: PDR and PODR for output, PCR only for pull-up input" {
    try std.testing.expectEqual(@as(u32, 0x5), gp.outputValue(true));
    try std.testing.expectEqual(@as(u32, 0x4), gp.outputValue(false));
    try std.testing.expectEqual(@as(u32, 0x10), gp.inputValue(1));
    try std.testing.expectEqual(@as(u32, 0), gp.inputValue(0));
    try std.testing.expectEqual(@as(u32, 0), gp.inputValue(2));
}

test "peripheral route clears PMR, writes PSEL, then sets PMR" {
    const s = gp.routeSteps(0x04);
    try std.testing.expectEqualSlices(u32, &.{ 0, 0x0400_0000, 0x0401_0000 }, &s);
    try std.testing.expectEqual(@as(u32, 0x1F01_0000), gp.routeSteps(0x1F)[2]);
}

test "IRQn maps to ELC event n + 1" {
    try std.testing.expectEqual(@as(u16, 1), gp.irqEvent(0));
    try std.testing.expectEqual(@as(u16, 16), gp.irqEvent(gp.irq_num_max));
}

test "program writes between unlock and lock" {
    var f = Pfs{};
    gp.program(&f, 0x4040_0804, &gp.routeSteps(0x04));
    try std.testing.expectEqualSlices(u8, &.{ 8, 8, 8, 8, 32, 32, 32, 8, 8, 8, 8 }, f.order[0..f.n]);
    try std.testing.expectEqualSlices(u32, &.{ 0, 0x0400_0000, 0x0401_0000 }, f.vals[0..f.n32]);
}
