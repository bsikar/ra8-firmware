//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_init.zig (RA8FW-868).

const std = @import("std");
const ini = @import("xspi_init");

const Regs = struct {
    mem: [0x200 / 4]u32 = [_]u32{0} ** (0x200 / 4),
    writes: usize = 0,
    spins: usize = 0,
    lioctl: [2]u32 = .{ 0, 0 },

    pub fn write(self: *Regs, off: usize, v: u32) void {
        if (off == ini.off_lioctl and self.spins < 2) self.lioctl[self.spins] = v;
        self.mem[off / 4] = v;
        self.writes += 1;
    }
    pub fn spin(self: *Regs) void {
        self.spins += 1;
    }
};

const Hw = struct {
    r: *Regs,
    trace: std.BoundedArray(u8, 32) = .{},
    srdy_ok: bool = true,
    mstp_rc: u16 = 0,

    fn mark(self: *Hw, c: u8) void {
        self.trace.append(c) catch unreachable;
    }
    pub fn prcr(self: *Hw, v: u16) void {
        self.mark(if (v == ini.prcr_unlock_cgc) 'U' else 'L');
    }
    pub fn writeDivcr(self: *Hw, _: u8) void {
        self.mark('d');
    }
    pub fn writeCkcr(self: *Hw, _: u8) void {
        self.mark('c');
    }
    pub fn waitSrdy(self: *Hw, _: bool) bool {
        self.mark('w');
        return self.srdy_ok;
    }
    pub fn mstpEnable(self: *Hw, _: u16) u16 {
        self.mark('M');
        return self.mstp_rc;
    }
    pub fn regs(self: *Hw, _: u8) *Regs {
        self.mark('R');
        return self.r;
    }
    pub fn info(self: *Hw, _: [*:0]const u8) void {
        self.mark('i');
    }
    pub fn infoVal(self: *Hw, _: [*:0]const u8, _: u32) void {
        self.mark('v');
    }
    pub fn err(self: *Hw, _: [*:0]const u8) void {
        self.mark('e');
    }
    pub fn fail(self: *Hw, _: [*:0]const u8, _: u16) void {
        self.mark('f');
    }
};

test "init runs the clock once, then MSTP, config and reset" {
    var r = Regs{};
    var hw = Hw{ .r = &r };
    var done = false;
    try std.testing.expectEqual(@as(u16, 0), ini.init(&hw, 1, 0x3FF, &done));
    try std.testing.expectEqualStrings("UdcwcwLiMRv", hw.trace.slice());
    try std.testing.expectEqual(@as(u32, 0x3FF), r.mem[ini.off_liocfg1 / 4]);
    try std.testing.expectEqual(@as(u32, 8), r.mem[ini.off_cdctl0 / 4]);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), r.mem[0x194 / 4]);
    try std.testing.expectEqual(@as(u32, 1), r.lioctl[0]);
    try std.testing.expectEqual(@as(u32, 0x0001_0001), r.lioctl[1]);
    try std.testing.expectEqual(@as(usize, 2), r.spins);
    hw.trace.len = 0;
    try std.testing.expectEqual(@as(u16, 0), ini.init(&hw, 0, 0x3FF, &done));
    try std.testing.expectEqualStrings("MRv", hw.trace.slice());
}

test "an SRDY timeout re-locks PRCR and leaves the clock un-inited" {
    var r = Regs{};
    var hw = Hw{ .r = &r, .srdy_ok = false };
    var done = false;
    try std.testing.expectEqual(ini.hw_timeout, ini.init(&hw, 0, 0, &done));
    try std.testing.expectEqualStrings("UdcweL", hw.trace.slice());
    try std.testing.expect(!done);
}

test "init rejects a bad instance and reports an MSTP failure" {
    var r = Regs{};
    var hw = Hw{ .r = &r };
    var done = true;
    try std.testing.expectEqual(ini.null_ptr, ini.init(&hw, 2, 0, &done));
    try std.testing.expectEqualStrings("e", hw.trace.slice());
    hw.trace.len = 0;
    hw.mstp_rc = 0x301;
    try std.testing.expectEqual(@as(u16, 0x301), ini.init(&hw, 0, 0, &done));
    try std.testing.expectEqualStrings("Mf", hw.trace.slice());
    try std.testing.expectEqual(@as(usize, 0), r.writes);
}

test "packCommand stores little-endian words plus a final partial" {
    var r = Regs{};
    ini.packCommand(&r, &.{ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 });
    try std.testing.expectEqual(@as(u32, 0x0403_0201), r.mem[ini.off_cdbuf / 4]);
    try std.testing.expectEqual(@as(u32, 0x0000_0605), r.mem[ini.off_cdbuf / 4 + 1]);
    try std.testing.expectEqual(@as(usize, 2), r.writes);
    var e = Regs{};
    ini.packCommand(&e, &.{});
    try std.testing.expectEqual(@as(usize, 0), e.writes);
}

test "deinit clears LIOCFGCS0 and INTE and every pending flag" {
    var r = Regs{};
    r.mem[ini.off_liocfg0 / 4] = 5;
    r.mem[ini.off_inte / 4] = 7;
    ini.deinit(&r);
    try std.testing.expectEqual(@as(u32, 0), r.mem[ini.off_liocfg0 / 4]);
    try std.testing.expectEqual(@as(u32, 0), r.mem[ini.off_inte / 4]);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), r.mem[0x194 / 4]);
}
