//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/reset.zig against a fake SYSC register file.

const std = @import("std");
const rs = @import("reset");
const off = rs.off;

const Regs = struct {
    bytes: [0xB00]u8 = [_]u8{0} ** 0xB00,
    prcr_writes: [4]u16 = undefined,
    nprcr: usize = 0,

    pub fn read8(self: *Regs, o: usize) u8 {
        return self.bytes[o];
    }
    pub fn write8(self: *Regs, o: usize, v: u8) void {
        self.bytes[o] = v;
    }
    pub fn read16(self: *Regs, o: usize) u16 {
        return std.mem.readInt(u16, self.bytes[o..][0..2], .little);
    }
    pub fn write16(self: *Regs, o: usize, v: u16) void {
        if (o == off.prcr) {
            self.prcr_writes[self.nprcr] = v;
            self.nprcr += 1;
        }
        std.mem.writeInt(u16, self.bytes[o..][0..2], v, .little);
    }
    pub fn read32(self: *Regs, o: usize) u32 {
        return std.mem.readInt(u32, self.bytes[o..][0..4], .little);
    }
    pub fn write32(self: *Regs, o: usize, v: u32) void {
        std.mem.writeInt(u32, self.bytes[o..][0..4], v, .little);
    }
};

test "Raw matches ra8_reset_raw_t and AIRCR carries VECTKEY plus SYSRESETREQ" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(rs.Raw));
    try std.testing.expectEqual(@as(u32, 0x05FA_0004), rs.aircr_reset);
    try std.testing.expectEqual(@as(usize, 0xE000_ED0C), rs.aircr_addr);
}

test "readRaw reads RSTSR0..3 from their SYSC offsets" {
    var r = Regs{};
    r.write8(off.rstsr0, 0x11);
    r.write32(off.rstsr1, 0x0040_0004);
    r.write8(off.rstsr2, 0x01);
    r.write8(off.rstsr3, 0x80);
    const raw = rs.readRaw(&r);
    try std.testing.expectEqual(@as(u8, 0x11), raw.rstsr0);
    try std.testing.expectEqual(@as(u32, 0x0040_0004), raw.rstsr1);
    try std.testing.expectEqual(@as(u8, 0x01), raw.rstsr2);
    try std.testing.expectEqual(@as(u8, 0x80), raw.rstsr3);
}

test "decode priority: RSTSR0, then RSTSR3, then RSTSR1, then CWSF" {
    const all = rs.Raw{ .rstsr0 = 0x80, .rstsr1 = 0x1, .rstsr2 = 1, .rstsr3 = 0x10 };
    try std.testing.expectEqual(@as(u8, 7), rs.decode(all)); // DPSRSTF
    try std.testing.expectEqual(@as(u8, 1), rs.decode(.{ .rstsr0 = 0xFF })); // PORF first
    try std.testing.expectEqual(@as(u8, 5), rs.decode(.{ .rstsr0 = 0x20 })); // LVD4RF
    try std.testing.expectEqual(@as(u8, 9), rs.decode(.{ .rstsr1 = 0x1, .rstsr3 = 0x90 })); // OCPRF before TEMPRF
    try std.testing.expectEqual(@as(u8, 10), rs.decode(.{ .rstsr3 = 0x80 }));
    try std.testing.expectEqual(@as(u8, 11), rs.decode(.{ .rstsr1 = 0x7F_FFFF, .rstsr2 = 1 })); // IWDT
    try std.testing.expectEqual(@as(u8, 16), rs.decode(.{ .rstsr1 = 0x0040_0400 })); // BUSS before NW
    try std.testing.expectEqual(@as(u8, 21), rs.decode(.{ .rstsr1 = 0x0040_0000 }));
    try std.testing.expectEqual(rs.cause.warm_start, rs.decode(.{ .rstsr2 = 1 }));
    try std.testing.expectEqual(rs.cause.unknown, rs.decode(.{ .rstsr0 = 0x10, .rstsr1 = 0x8, .rstsr3 = 0x6E }));
}

test "clear writes 0 over selected RSTSR0/RSTSR1 flags and 1 to CWSF" {
    var r = Regs{};
    r.write8(off.rstsr0, 0xFF);
    r.write32(off.rstsr1, 0x0060_0007);
    rs.clear(&r, 0x8000_0000 | (0x0020_0002 << 8) | 0x81);
    try std.testing.expectEqual(@as(u8, 0x7E), r.read8(off.rstsr0));
    try std.testing.expectEqual(@as(u32, 0x0040_0005), r.read32(off.rstsr1));
    try std.testing.expectEqual(@as(u8, 0x01), r.read8(off.rstsr2));
}

test "clear with an empty mask writes nothing" {
    var r = Regs{};
    r.write8(off.rstsr0, 0xFF);
    r.write32(off.rstsr1, 0xFFFF_FFFF);
    rs.clear(&r, 0);
    try std.testing.expectEqual(@as(u8, 0xFF), r.read8(off.rstsr0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), r.read32(off.rstsr1));
    try std.testing.expectEqual(@as(u8, 0), r.read8(off.rstsr2));
}

test "sourceLoc maps every ra8_reset_source_t and rejects count" {
    const want = [_]rs.Loc{
        .{ .reg = off.syrstmsk0, .mask = 0x01 }, .{ .reg = off.syrstmsk0, .mask = 0x02 },
        .{ .reg = off.syrstmsk0, .mask = 0x04 }, .{ .reg = off.syrstmsk0, .mask = 0x10 },
        .{ .reg = off.syrstmsk0, .mask = 0x20 }, .{ .reg = off.syrstmsk0, .mask = 0x40 },
        .{ .reg = off.syrstmsk0, .mask = 0x80 }, .{ .reg = off.syrstmsk1, .mask = 0x02 },
        .{ .reg = off.syrstmsk1, .mask = 0x10 }, .{ .reg = off.syrstmsk1, .mask = 0x20 },
        .{ .reg = off.syrstmsk2, .mask = 0x01 }, .{ .reg = off.syrstmsk2, .mask = 0x02 },
    };
    for (want, 0..) |w, i| try std.testing.expectEqual(w, rs.sourceLoc(@intCast(i)).?);
    try std.testing.expect(rs.sourceLoc(12) == null);
    try std.testing.expect(rs.sourceLoc(255) == null);
}

test "setSourceMask unlocks PRC5, flips only the bit, relocks keeping PR bits" {
    var r = Regs{};
    r.write16(off.prcr, 0x0003);
    r.nprcr = 0;
    r.write8(off.syrstmsk1, 0x41);
    const loc = rs.sourceLoc(8).?; // CLU1
    rs.setSourceMask(&r, loc, true);
    try std.testing.expectEqual(@as(u8, 0x51), r.read8(off.syrstmsk1));
    try std.testing.expect(rs.sourceMasked(&r, loc));
    try std.testing.expectEqual(@as(usize, 2), r.nprcr);
    try std.testing.expectEqual(@as(u16, 0xA523), r.prcr_writes[0]);
    try std.testing.expectEqual(@as(u16, 0xA503), r.prcr_writes[1]);
    rs.setSourceMask(&r, loc, false);
    try std.testing.expectEqual(@as(u8, 0x41), r.read8(off.syrstmsk1));
    try std.testing.expect(!rs.sourceMasked(&r, loc));
}
