//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rmac_phy.zig against a fake Clause 22 PHY.

const std = @import("std");
const phy = @import("rmac_phy");
const codes = phy.codes;

/// A PHY register file. `reset_reads` is how many BMCR reads still show
/// RESET set; `fail_read_at` fails the Nth read (1-based) with 0x203.
const Fake = struct {
    regs: [32]u16 = @splat(0),
    reset_reads: u32 = 0,
    reads: u32 = 0,
    writes: u32 = 0,
    last_reg: u8 = 0xFF,
    last_value: u16 = 0,
    fail_read_at: u32 = 0,
    fail_write: bool = false,
    logs: u32 = 0,

    pub fn read(self: *Fake, _: u8, _: u8, reg: u8, out: *u16) u16 {
        self.reads += 1;
        if (self.reads == self.fail_read_at) return 0x203;
        if (reg == phy.reg_bmcr and self.reset_reads > 0) {
            self.reset_reads -= 1;
            out.* = phy.bmcr_reset;
            return 0;
        }
        out.* = self.regs[reg];
        return 0;
    }
    pub fn write(self: *Fake, _: u8, _: u8, reg: u8, value: u16) u16 {
        self.writes += 1;
        if (self.fail_write) return 0x203;
        self.last_reg = reg;
        self.last_value = value;
        self.regs[reg] = value & ~phy.bmcr_reset;
        return 0;
    }
    pub fn logError(self: *Fake, _: [*:0]const u8) void {
        self.logs += 1;
    }
};

test "bad port or PHY address is refused and logged before any MDIO" {
    var f: Fake = .{};
    var link: phy.Link = .{ .up = true, .speed = 4 };
    try std.testing.expectEqual(codes.invalid_arg, phy.reset(&f, 2, 1));
    try std.testing.expectEqual(codes.invalid_arg, phy.setAdvertise(&f, 0, 32, 0));
    try std.testing.expectEqual(codes.invalid_arg, phy.autoNegStart(&f, 9, 0));
    try std.testing.expectEqual(codes.invalid_arg, phy.autoNegWait(&f, 0, 40, 10, &link));
    try std.testing.expectEqual(codes.invalid_arg, phy.linkStatus(&f, 3, 0, &link));
    try std.testing.expectEqual(codes.null_ptr, phy.autoNegWait(&f, 0, 1, 10, null));
    try std.testing.expectEqual(codes.null_ptr, phy.linkStatus(&f, 0, 1, null));
    try std.testing.expectEqual(@as(u32, 7), f.logs);
    try std.testing.expectEqual(@as(u32, 0), f.reads + f.writes);
    try std.testing.expect(phy.argsOk(1, 31));
}

test "reset writes BMCR.RESET and polls until it clears" {
    var f: Fake = .{ .reset_reads = 3 };
    try std.testing.expectEqual(codes.ok, phy.reset(&f, 1, 31));
    try std.testing.expectEqual(phy.reg_bmcr, f.last_reg);
    try std.testing.expectEqual(phy.bmcr_reset, f.last_value);
    try std.testing.expectEqual(@as(u32, 4), f.reads);
    f = .{ .reset_reads = phy.reset_iter_cap };
    try std.testing.expectEqual(codes.hw_timeout, phy.reset(&f, 0, 1));
    try std.testing.expectEqual(phy.reset_iter_cap, f.reads);
    f = .{ .fail_write = true };
    try std.testing.expectEqual(@as(u16, 0x203), phy.reset(&f, 0, 1));
    try std.testing.expectEqual(@as(u32, 0), f.reads);
    f = .{ .reset_reads = 5, .fail_read_at = 2 };
    try std.testing.expectEqual(@as(u16, 0x203), phy.reset(&f, 0, 1));
    try std.testing.expectEqual(@as(u32, 1), f.logs);
}

test "advertise ORs the 802.3 selector, auto-neg start kicks BMCR" {
    var f: Fake = .{};
    try std.testing.expectEqual(codes.ok, phy.setAdvertise(&f, 0, 1, 0x01E0));
    try std.testing.expectEqual(phy.reg_anar, f.last_reg);
    try std.testing.expectEqual(@as(u16, 0x01E1), f.last_value);
    try std.testing.expectEqual(codes.ok, phy.autoNegStart(&f, 0, 1));
    try std.testing.expectEqual(phy.reg_bmcr, f.last_reg);
    try std.testing.expectEqual(@as(u16, 0x1200), f.last_value);
}

test "ANLPAR decodes to the best common mode" {
    try std.testing.expectEqual(phy.speed_100_fd, phy.decodeAnlpar(0x01E0));
    try std.testing.expectEqual(phy.speed_100_hd, phy.decodeAnlpar(0x00E0));
    try std.testing.expectEqual(phy.speed_10_fd, phy.decodeAnlpar(0x0060));
    try std.testing.expectEqual(phy.speed_10_hd, phy.decodeAnlpar(0x0020));
    try std.testing.expectEqual(phy.speed_unknown, phy.decodeAnlpar(0x001F));
}

test "the wait budget is timeout_ms * 100, 65536 for zero and one on wrap" {
    try std.testing.expectEqual(@as(u32, 1000), phy.waitBudget(10));
    try std.testing.expectEqual(phy.anwait_iter_cap, phy.waitBudget(0));
    // 2^30 * 100 = 25 * 2^32, which wraps to 0 in 32 bits.
    try std.testing.expectEqual(@as(u32, 1), phy.waitBudget(0x4000_0000));
}

test "auto-neg wait needs AN_DONE and LINK_UP, then reads the partner" {
    var f: Fake = .{};
    var link: phy.Link = .{ .up = true, .speed = 4 };
    f.regs[phy.reg_bmsr] = phy.bmsr_link_up;
    try std.testing.expectEqual(codes.hw_timeout, phy.autoNegWait(&f, 0, 1, 1, &link));
    try std.testing.expectEqual(@as(u32, 100), f.reads);
    try std.testing.expectEqual(phy.Link{ .up = false, .speed = phy.speed_unknown }, link);
    f = .{};
    f.regs[phy.reg_bmsr] = phy.bmsr_link_up | phy.bmsr_an_done;
    f.regs[phy.reg_anlpar] = phy.anlpar_100_hd | phy.anlpar_10_fd;
    try std.testing.expectEqual(codes.ok, phy.autoNegWait(&f, 1, 3, 0, &link));
    try std.testing.expectEqual(phy.Link{ .up = true, .speed = phy.speed_100_hd }, link);
    f.reads = 0;
    f.fail_read_at = 2;
    try std.testing.expectEqual(@as(u16, 0x203), phy.autoNegWait(&f, 1, 3, 0, &link));
    try std.testing.expect(!link.up);
    f.reads = 0;
    f.fail_read_at = 1;
    try std.testing.expectEqual(@as(u16, 0x203), phy.autoNegWait(&f, 1, 3, 0, &link));
}

test "link status reads ANLPAR only when the link is up and AN is done" {
    var f: Fake = .{};
    var link: phy.Link = .{ .up = true, .speed = 4 };
    try std.testing.expectEqual(codes.ok, phy.linkStatus(&f, 0, 1, &link));
    try std.testing.expectEqual(phy.Link{ .up = false, .speed = phy.speed_unknown }, link);
    f.regs[phy.reg_bmsr] = phy.bmsr_link_up;
    f.regs[phy.reg_anlpar] = phy.anlpar_100_fd;
    f.reads = 0;
    try std.testing.expectEqual(codes.ok, phy.linkStatus(&f, 0, 1, &link));
    try std.testing.expectEqual(phy.Link{ .up = true, .speed = phy.speed_unknown }, link);
    try std.testing.expectEqual(@as(u32, 1), f.reads);
    f.regs[phy.reg_bmsr] |= phy.bmsr_an_done;
    try std.testing.expectEqual(codes.ok, phy.linkStatus(&f, 0, 1, &link));
    try std.testing.expectEqual(phy.Link{ .up = true, .speed = phy.speed_100_fd }, link);
    f.reads = 0;
    f.fail_read_at = 1;
    try std.testing.expectEqual(@as(u16, 0x203), phy.linkStatus(&f, 0, 1, &link));
    f.reads = 0;
    f.fail_read_at = 2;
    try std.testing.expectEqual(@as(u16, 0x203), phy.linkStatus(&f, 0, 1, &link));
}
