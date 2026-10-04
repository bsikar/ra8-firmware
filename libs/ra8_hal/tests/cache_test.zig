//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/cache.zig (RA8FW-612). The fake records every
//! register write and barrier in order.

const std = @import("std");
const c = @import("cache");

const Op = struct { kind: u8, addr: usize = 0, value: u32 = 0 };

const Fake = struct {
    ctr_val: u32 = 3 << 16,
    ccsidr_val: u32 = 0,
    ccr_val: u32 = 0,
    log: [256]Op = undefined,
    n: usize = 0,
    errors: usize = 0,
    last_err: []const u8 = "",

    fn push(self: *Fake, op: Op) void {
        if (self.n < self.log.len) self.log[self.n] = op;
        self.n += 1;
    }
    pub fn read32(self: *Fake, addr: usize) u32 {
        return switch (addr) {
            c.ctr => self.ctr_val,
            c.ccsidr => self.ccsidr_val,
            c.ccr => self.ccr_val,
            else => 0,
        };
    }
    pub fn write32(self: *Fake, addr: usize, v: u32) void {
        if (addr == c.ccr) self.ccr_val = v;
        self.push(.{ .kind = 'w', .addr = addr, .value = v });
    }
    pub fn dsb(self: *Fake) void {
        self.push(.{ .kind = 'd' });
    }
    pub fn isb(self: *Fake) void {
        self.push(.{ .kind = 'i' });
    }
    pub fn err(self: *Fake, msg: [*:0]const u8) void {
        self.errors += 1;
        self.last_err = std.mem.span(msg);
    }
    fn kinds(self: *Fake, buf: []u8) []const u8 {
        for (self.log[0..self.n], 0..) |op, i| buf[i] = op.kind;
        return buf[0..self.n];
    }
};

test "line bytes come from CTR.DminLine" {
    var f = Fake{};
    try std.testing.expectEqual(@as(u32, 32), c.lineBytes(&f));
    f.ctr_val = 0xFFFF_FFFF;
    try std.testing.expectEqual(@as(u32, 4 << 15), c.lineBytes(&f));
}

test "span aligns both ends to the line" {
    const s = c.span(0x2000_0010, 0x40, 32);
    try std.testing.expectEqual(@as(usize, 0x2000_0000), s.start);
    try std.testing.expectEqual(@as(u32, 3), s.lines);
    try std.testing.expectEqual(@as(u32, 1), c.span(0x100, 1, 32).lines);
    try std.testing.expectEqual(@as(u32, 1), c.span(0x100, 32, 32).lines);
}

test "range null logs and returns null_ptr, size 0 touches nothing" {
    var f = Fake{};
    try std.testing.expectEqual(c.null_ptr, c.maintainRange(&f, 0, 4, c.dccmvac));
    try std.testing.expectEqualStrings("maintain: addr", f.last_err);
    try std.testing.expectEqual(c.ok, c.maintainRange(&f, 0x2000_0000, 0, c.dccmvac));
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "range writes each line address between barriers" {
    var f = Fake{};
    try std.testing.expectEqual(c.ok, c.maintainRange(&f, 0x3000_0021, 0x40, c.dcimvac));
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualStrings("dwwwdi", f.kinds(&buf));
    try std.testing.expectEqual(c.dcimvac, f.log[1].addr);
    try std.testing.expectEqual(@as(u32, 0x3000_0020), f.log[1].value);
    try std.testing.expectEqual(@as(u32, 0x3000_0060), f.log[3].value);
}

test "setway bails on missing geometry" {
    var f = Fake{};
    c.setwayAll(&f, c.dcisw);
    var buf: [8]u8 = undefined;
    try std.testing.expectEqualStrings("wd", f.kinds(&buf));
    f = Fake{ .ccsidr_val = 0xFFFF_FFFF };
    c.setwayAll(&f, c.dcisw);
    try std.testing.expectEqual(@as(usize, 2), f.n);
}

test "setway walks sets and ways down to zero inclusive" {
    var f = Fake{ .ccsidr_val = (1 << 13) | (3 << 3) };
    c.setwayAll(&f, c.dccisw);
    try std.testing.expectEqual(@as(usize, 2 + 8 + 2), f.n);
    try std.testing.expectEqual(@as(u32, (1 << 5) | (3 << 30)), f.log[2].value);
    try std.testing.expectEqual(@as(u32, 0), f.log[9].value);
    try std.testing.expectEqual(c.dccisw, f.log[9].addr);
}

test "icache enable and disable flip CCR.IC around ICIALLU" {
    var f = Fake{};
    c.icacheEnable(&f);
    try std.testing.expectEqual(c.ccr_ic, f.ccr_val);
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualStrings("diwdiwdi", f.kinds(&buf));
    f.n = 0;
    c.icacheDisable(&f);
    try std.testing.expectEqual(@as(u32, 0), f.ccr_val);
    try std.testing.expectEqualStrings("diwwdi", f.kinds(&buf));
    try std.testing.expectEqual(c.ccr, f.log[2].addr);
    try std.testing.expectEqual(c.iciallu, f.log[3].addr);
}

test "dcache enable invalidates first, disable cleans after" {
    var f = Fake{ .ccsidr_val = 0, .ccr_val = 0x200 };
    c.dcacheEnable(&f);
    try std.testing.expectEqual(@as(u32, 0x200) | c.ccr_dc, f.ccr_val);
    try std.testing.expectEqual(c.csselr, f.log[0].addr);
    c.dcacheDisable(&f);
    try std.testing.expectEqual(@as(u32, 0x200), f.ccr_val);
}
