//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Simple LIN logic (RA8FW-754): PID parity, checksums, header checks, cfg
//! validation and the mode programming order.

const std = @import("std");
const lin = @import("sci_lin");

const Rec = struct {
    ccr3: u32 = 0xFFFF_FFFF,
    addrs: [8]usize = undefined,
    vals: [8]u32 = undefined,
    n: usize = 0,
    pub fn read32(self: *Rec, addr: usize) u32 {
        _ = addr;
        return self.ccr3;
    }
    pub fn write32(self: *Rec, addr: usize, value: u32) void {
        self.addrs[self.n] = addr;
        self.vals[self.n] = value;
        self.n += 1;
    }
};

fn cfg(role: u8, clk: u8, brk: u16) lin.Cfg {
    return .{ .uart = .{ .baud = 19200, .data_bits = 8, .parity = 0, .stop_bits = 0, .pclk_hz = 0 }, .role = role, .timer_clk = clk, .break_field_len = brk };
}

test "pid matches the LIN 2.x parity table" {
    try std.testing.expectEqual(@as(u8, 0x80), lin.pid(0x00));
    try std.testing.expectEqual(@as(u8, 0xC1), lin.pid(0x01));
    try std.testing.expectEqual(@as(u8, 0x3C), lin.pid(0x3C));
    try std.testing.expectEqual(@as(u8, 0x7D), lin.pid(0x3D));
    try std.testing.expectEqual(lin.pid(0x05), lin.pid(0x45));
}

test "classic and enhanced checksums fold the carries" {
    const data = [_]u8{ 0x4A, 0x55, 0x93, 0xE5 };
    try std.testing.expectEqual(@as(u8, 0xE6), lin.checksum(lin.checksum_classic, 0, &data));
    try std.testing.expectEqual(@as(u8, 0x96), lin.checksum(lin.checksum_enhanced, 0x50, &data));
    try std.testing.expectEqual(@as(u8, 0xFF), lin.checksum(lin.checksum_classic, 0x12, &.{}));
    const full = [_]u8{0xFF} ** 8;
    try std.testing.expectEqual(@as(u8, 0x00), lin.checksum(lin.checksum_classic, 0, &full));
}

test "fold handles a carry out of the first pass" {
    try std.testing.expectEqual(@as(u8, 0xFE), lin.foldComplement(0x01FF));
    try std.testing.expectEqual(@as(u8, 0x00), lin.foldComplement(0xFFFF));
}

test "header check wants 0x55 and a matching parity" {
    const good = lin.checkHeader(0x55, lin.pid(0x10));
    try std.testing.expect(good.valid);
    try std.testing.expectEqual(@as(u8, 0x10), good.id);
    try std.testing.expect(!lin.checkHeader(0x54, lin.pid(0x10)).valid);
    const bad = lin.checkHeader(0x55, lin.pid(0x10) ^ 0x80);
    try std.testing.expect(!bad.valid);
    try std.testing.expectEqual(@as(u8, 0x10), bad.id);
}

test "cfg checks role, break length and timer clock" {
    try std.testing.expect(lin.cfgOk(cfg(lin.role_commander, lin.clk_div4, 13)));
    try std.testing.expect(lin.cfgOk(cfg(lin.role_responder, lin.clk_div64, lin.xcr2_bflw_max)));
    try std.testing.expect(!lin.cfgOk(cfg(2, lin.clk_div4, 13)));
    try std.testing.expect(!lin.cfgOk(cfg(lin.role_commander, lin.clk_div4, 0xFFFF)));
    try std.testing.expect(!lin.cfgOk(cfg(lin.role_commander, 0, 13)));
    try std.testing.expect(!lin.cfgOk(cfg(lin.role_commander, 4, 13)));
}

test "commander mode programs CCR0 last with TE and RE" {
    var r = Rec{};
    lin.programMode(&r, 2, lin.role_commander, 2, 0x20);
    const b = lin.regAddr(2, 0);
    try std.testing.expectEqual(@as(usize, 6), r.n);
    try std.testing.expectEqual(b + lin.off_ccr0, r.addrs[0]);
    try std.testing.expectEqual(@as(u32, 0), r.vals[0]);
    try std.testing.expectEqual(b + lin.off_ccr3, r.addrs[1]);
    try std.testing.expectEqual(@as(u32, 0xFFFE_FFFF), r.vals[1]);
    try std.testing.expectEqual(@as(u32, 0x102), r.vals[2]);
    try std.testing.expectEqual(@as(u32, 0x0020_0000), r.vals[3]);
    try std.testing.expectEqual(b + lin.off_xcr1, r.addrs[4]);
    try std.testing.expectEqual(@as(u32, 0), r.vals[4]);
    try std.testing.expectEqual(b + lin.off_ccr0, r.addrs[5]);
    try std.testing.expectEqual(@as(u32, 0x11), r.vals[5]);
}

test "responder mode turns on start frame detect and bit rate measure" {
    var r = Rec{ .ccr3 = 0 };
    lin.programMode(&r, 0, lin.role_responder, lin.clk_div4, 13);
    try std.testing.expectEqual(@as(u32, 0x0006_0000), r.vals[1]);
    try std.testing.expectEqual(@as(u32, 0x30), r.vals[4]);
    try std.testing.expectEqual(@as(usize, 0x40358000 + 0x38), r.addrs[4]);
    try std.testing.expect(!lin.channelOk(10));
}
