//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_program.zig (RA8FW-871).

const std = @import("std");
const pg = @import("xspi_program");

const off_cdctl0 = 0x070;
const off_ints = 0x190;

/// NOR model: logs each kick's CDT opcode, CDA and CDD0/CDD1 as seen AT
/// the kick; RDSR reports WIP for `busy` polls. `fail_on` times out a kick.
const Regs = struct {
    mem: [0x200 / 4]u32 = [_]u32{0} ** (0x200 / 4),
    kicks: u32 = 0,
    fail_on: ?u32 = null,
    busy: u32 = 0,
    ops: [16]u8 = [_]u8{0} ** 16,
    addrs: [16]u32 = [_]u32{0} ** 16,
    lo: [16]u32 = [_]u32{0} ** 16,
    sizes: [16]u32 = [_]u32{0} ** 16,
    hi: [16]u32 = [_]u32{0} ** 16,

    pub fn read(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        if (off != off_cdctl0 or v & 1 == 0) return;
        const op: u8 = @truncate(self.mem[0x80 / 4] >> 24);
        if (op != 0x05 and self.kicks < self.ops.len) {
            const k = self.kicks;
            self.ops[k] = op;
            self.addrs[k] = self.mem[0x84 / 4];
            self.lo[k] = self.mem[0x88 / 4];
            self.hi[k] = self.mem[0x8C / 4];
            self.sizes[k] = (self.mem[0x80 / 4] >> 5) & 0xF;
        }
        self.kicks += 1;
        if (op == 0x05) {
            self.mem[0x88 / 4] = if (self.busy > 0) 1 else 0;
            if (self.busy > 0) self.busy -= 1;
        }
    }
    pub fn poll(self: *Regs, _: u32, _: bool) bool {
        if (self.fail_on) |k| if (self.kicks == k) return false;
        self.mem[off_ints / 4] = 1;
        return true;
    }
};

test "programChunk stages the payload before TRREQ" {
    var r = Regs{};
    try std.testing.expectEqual(@as(u16, 0), pg.programChunk(&r, 0x100, &.{ 1, 2, 3, 4, 5 }));
    try std.testing.expectEqual(@as(u8, 0x06), r.ops[0]);
    try std.testing.expectEqual(@as(u8, 0x02), r.ops[1]);
    try std.testing.expectEqual(@as(u32, 0x100), r.addrs[1]);
    try std.testing.expectEqual(@as(u32, 0x0403_0201), r.lo[1]);
    try std.testing.expectEqual(@as(u32, 5), r.hi[1]);
}

test "program splits at the 256-byte page and the 8-byte slot" {
    var r = Regs{};
    const data = [_]u8{0xAA} ** 12;
    try std.testing.expectEqual(@as(u16, 0), pg.program(&r, 0xFC, &data));
    // WREN, PP, RDSR per chunk; chunks at 0xFC (4 bytes), 0x100 (8 bytes).
    try std.testing.expectEqual(@as(u32, 6), r.kicks);
    try std.testing.expectEqual(@as(u32, 0xFC), r.addrs[1]);
    try std.testing.expectEqual(@as(u32, 4), r.sizes[1]);
    try std.testing.expectEqual(@as(u32, 0x100), r.addrs[4]);
    try std.testing.expectEqual(@as(u32, 8), r.sizes[4]);
}

test "pollWipClear waits out WIP and times out when it stays set" {
    var r = Regs{ .busy = 3 };
    try std.testing.expectEqual(@as(u16, 0), pg.pollWipClear(&r));
    try std.testing.expectEqual(@as(u32, 4), r.kicks);
    r.busy = pg.wip_polls;
    try std.testing.expectEqual(pg.timeout, pg.pollWipClear(&r));
}

test "eraseSector sends WREN then 0x20 with no data" {
    var r = Regs{};
    try std.testing.expectEqual(@as(u16, 0), pg.eraseSector(&r, 0x1000));
    try std.testing.expectEqual(@as(u8, 0x20), r.ops[1]);
    try std.testing.expectEqual(@as(u32, 0x1000), r.addrs[1]);
    try std.testing.expectEqual(@as(u32, 0), r.sizes[1]);
}

test "a WREN timeout or an out-of-range address stops early" {
    var r = Regs{ .fail_on = 1 };
    try std.testing.expectEqual(@as(u16, 0x203), pg.eraseSector(&r, 0));
    try std.testing.expectEqual(@as(u32, 1), r.kicks);
    var q = Regs{};
    try std.testing.expectEqual(@as(u16, 0x103), pg.program(&q, 0xFF_FFFF, &.{ 1, 2 }));
    try std.testing.expectEqual(@as(u32, 0), q.kicks);
}
