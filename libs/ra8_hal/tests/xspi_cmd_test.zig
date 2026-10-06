//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_cmd.zig (RA8FW-869).

const std = @import("std");
const cmd = @import("xspi_cmd");

const Regs = struct {
    mem: [0x200 / 4]u32 = @splat(0),
    done_at: ?u32 = 0,
    reply: u32 = 0,

    pub fn read(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        if (off == cmd.off_cdctl0 and v & cmd.cdctl0_trreq != 0) {
            self.mem[cmd.cdbuf(2) / 4] = self.reply;
        }
    }
    pub fn poll(self: *Regs, iter: u32, _: bool) bool {
        const at = self.done_at orelse return false;
        if (iter < at) return false;
        self.mem[cmd.off_ints / 4] = 0x11;
        return true;
    }
};

test "makeCdt packs a 1-byte opcode into CMD's upper byte" {
    try std.testing.expectEqual(@as(u32, 0x0500_0021), cmd.makeCdt(0x05, 1, 0, 1, 0));
    try std.testing.expectEqual(@as(u32, 0x9F00_0061), cmd.makeCdt(0x9F, 1, 0, 3, 0));
    try std.testing.expectEqual(@as(u32, 0x0002_800E), cmd.makeCdt(0x02, 2, 3, 0, 1));
    try std.testing.expectEqual(@as(u32, 0x0000_0003), cmd.makeCdt(0xAA, 3, 0, 0, 0));
}

test "issue writes slot 0, sets TRREQ and clears INTS on completion" {
    var r = Regs{};
    r.mem[cmd.cdbuf(1) / 4] = 7;
    try std.testing.expectEqual(@as(u16, 0), cmd.issue(&r, 0x06, 0));
    try std.testing.expectEqual(cmd.makeCdt(0x06, 1, 0, 0, 0), r.mem[cmd.cdbuf(0) / 4]);
    try std.testing.expectEqual(@as(u32, 0), r.mem[cmd.cdbuf(1) / 4]);
    try std.testing.expectEqual(@as(u32, 1), r.mem[cmd.off_cdctl0 / 4]);
    try std.testing.expectEqual(@as(u32, 0x11), r.mem[cmd.off_intc / 4]);
}

test "a CMDCMP timeout returns 0x203 and leaves INTC alone" {
    var r = Regs{ .done_at = null };
    try std.testing.expectEqual(cmd.hw_timeout, cmd.kick(&r));
    try std.testing.expectEqual(@as(u32, 0), r.mem[cmd.off_intc / 4]);
}

test "readStatus returns CDD0's low byte" {
    var r = Regs{ .reply = 0xABCD_0103, .done_at = 3 };
    var s: u8 = 0;
    try std.testing.expectEqual(@as(u16, 0), cmd.readStatus(&r, &s));
    try std.testing.expectEqual(@as(u8, 0x03), s);
}

test "readId reorders the JEDEC bytes to MFR, TYPE, CAP" {
    var r = Regs{ .reply = 0x001A_609D };
    var id: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0), cmd.readId(&r, &id));
    try std.testing.expectEqual(@as(u32, 0x9D_601A), id);
    var t = Regs{ .done_at = null };
    try std.testing.expectEqual(cmd.hw_timeout, cmd.readId(&t, &id));
}
