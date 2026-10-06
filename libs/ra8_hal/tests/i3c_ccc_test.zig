//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_ccc.zig.

const std = @import("std");
const ccc = @import("i3c_ccc");

/// NCMDQP/NTDTBP0 writes are logged; NTDTBP0 reads pop `rx`; NTST is a cell.
const Regs = struct {
    log: [16][2]u32 = undefined,
    n: usize = 0,
    rx: []const u32 = &.{},
    ntst: u32 = 0xFF,
    pub fn read32(self: *Regs, off: usize) u32 {
        if (off == ccc.off_ntst) return self.ntst;
        const w = self.rx[0];
        self.rx = self.rx[1..];
        return w;
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        if (off == ccc.off_ntst) {
            self.ntst = v;
            return;
        }
        self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
    }
};

fn expectLog(r: *const Regs, want: []const [2]u32) !void {
    try std.testing.expectEqual(want.len, r.n);
    for (want, 0..) |w, i| try std.testing.expectEqual(w, r.log[i]);
    try std.testing.expectEqual(@as(u32, 0xF7), r.ntst);
}

test "CCC and ENTDAA descriptor words" {
    try std.testing.expectEqual(@as(u32, 0xC000_8300), ccc.cccWord(0x06, 0, false));
    try std.testing.expectEqual(@as(u32, 0xE042_C380), ccc.cccWord(0x87, 0x42, true));
    try std.testing.expectEqual(@as(u32, 0xCC00_0382), ccc.entdaaWord(3));
}

test "DAA drains PID, BCR and DCR per target and keeps the address" {
    var r = Regs{ .rx = &.{ 0x4433_2211, 0x7766_6655, 0x0D0C_0B0A, 0x2120_0F0E } };
    var t = [_]ccc.Target{ .{ .pid = .{0} ** 6, .bcr = 0, .dcr = 0, .dynamic_address = 0x09 }, .{ .pid = .{0} ** 6, .bcr = 0, .dcr = 0, .dynamic_address = 0x0A } };
    ccc.daa(&r, &t);
    try expectLog(&r, &.{ .{ 0x150, ccc.entdaaWord(2) }, .{ 0x150, 0 } });
    try std.testing.expectEqualSlices(u8, &.{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, &t[0].pid);
    try std.testing.expectEqual(@as(u8, 0x66), t[0].bcr);
    try std.testing.expectEqual(@as(u8, 0x77), t[0].dcr);
    try std.testing.expectEqualSlices(u8, &.{ 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F }, &t[1].pid);
    try std.testing.expectEqual(@as(u8, 0x0A), t[1].dynamic_address);
}

test "SETDASA sends one immediate byte, RSTDAA a bare broadcast" {
    var r = Regs{};
    ccc.setdasa(&r, 0x50, 0x09);
    try expectLog(&r, &.{ .{ 0x150, ccc.cccWord(0x87, 0x50, false) | 1 | (1 << 23) }, .{ 0x150, 0x09 } });
    var z = Regs{};
    ccc.rstdaa(&z);
    try expectLog(&z, &.{ .{ 0x150, ccc.cccWord(0x06, 0, false) }, .{ 0x150, 0 } });
}

test "send packs up to four bytes in the descriptor, longer through the FIFO" {
    var r = Regs{};
    ccc.send(&r, 0x9A, 0x11, &.{ 1, 2, 3 });
    try expectLog(&r, &.{ .{ 0x150, ccc.cccWord(0x9A, 0x11, false) | 1 | (3 << 23) }, .{ 0x150, 0x0003_0201 } });
    var f = Regs{};
    ccc.send(&f, 0x9A, 0x11, &.{ 1, 2, 3, 4, 5, 6 });
    try expectLog(&f, &.{
        .{ 0x150, ccc.cccWord(0x9A, 0x11, false) },
        .{ 0x150, 6 << 16 },
        .{ 0x158, 0x0403_0201 },
        .{ 0x158, 0x0000_0605 },
    });
}

test "recv declares the length and reads the FIFO" {
    var r = Regs{ .rx = &.{ 0x4433_2211, 0x0000_0055 } };
    var buf: [5]u8 = undefined;
    ccc.recv(&r, 0x8B, 0x21, &buf);
    try expectLog(&r, &.{ .{ 0x150, ccc.cccWord(0x8B, 0x21, true) }, .{ 0x150, 5 << 16 } });
    try std.testing.expectEqualSlices(u8, &.{ 0x11, 0x22, 0x33, 0x44, 0x55 }, &buf);
}
