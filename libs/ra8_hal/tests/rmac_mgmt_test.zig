//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rmac_mgmt.zig against a fake RMAC register block.

const std = @import("std");
const mg = @import("rmac_mgmt");
const codes = mg.codes;
const off = mg.off;

/// A plain register file: each word starts at offset/4 + 1 so every read is
/// distinguishable; writes are recorded in order.
const Regs = struct {
    words: [0x600 / 4]u32 = undefined,
    writes: [16]usize = undefined,
    nwrites: usize = 0,

    fn init() Regs {
        var r: Regs = .{};
        for (&r.words, 0..) |*w, i| w.* = @intCast(i + 1);
        return r;
    }
    pub fn read32(self: *Regs, o: usize) u32 {
        return self.words[o / 4];
    }
    pub fn write32(self: *Regs, o: usize, v: u32) void {
        self.writes[self.nwrites] = o;
        self.nwrites += 1;
        self.words[o / 4] = v;
    }
    fn at(self: *const Regs, o: usize) u32 {
        return self.words[o / 4];
    }
};

const Log = struct {
    n: u32 = 0,
    pub fn logError(self: *Log, _: [*:0]const u8) void {
        self.n += 1;
    }
};

test "bad port and null out are refused and logged without touching registers" {
    var r = Regs.init();
    var log: Log = .{};
    var st: mg.Status = undefined;
    var stats: mg.Stats = undefined;
    try std.testing.expectEqual(codes.invalid_arg, mg.getStatus(&r, &log, 2, &st));
    try std.testing.expectEqual(codes.null_ptr, mg.getStatus(&r, &log, 0, null));
    try std.testing.expectEqual(codes.invalid_arg, mg.clearStatus(&r, &log, 7, 1, .{ 1, 1, 1 }));
    try std.testing.expectEqual(codes.invalid_arg, mg.readStats(&r, &log, 2, &stats));
    try std.testing.expectEqual(codes.null_ptr, mg.readStats(&r, &log, 1, null));
    try std.testing.expectEqual(@as(u32, 5), log.n);
    try std.testing.expectEqual(@as(usize, 0), r.nwrites);
}

test "get_status reads MEIS, MMIS0..2, MPIM and MRMAC0/1" {
    var r = Regs.init();
    var log: Log = .{};
    var st: mg.Status = undefined;
    try std.testing.expectEqual(codes.ok, mg.getStatus(&r, &log, 1, &st));
    try std.testing.expectEqual(r.at(off.meis), st.err_status);
    try std.testing.expectEqual([3]u32{ r.at(0x210), r.at(0x220), r.at(0x230) }, st.mon_status);
    try std.testing.expectEqual(r.at(off.mpim), st.phy_monitor);
    try std.testing.expectEqual(r.at(off.mrmac0), st.mrmac0);
    try std.testing.expectEqual(r.at(off.mrmac1), st.mrmac1);
}

test "clear_status writes the disables, then clears only the masked status bits" {
    var r = Regs.init();
    var log: Log = .{};
    r.words[off.meis / 4] = 0xFF;
    r.words[0x210 / 4] = 0x0F;
    r.words[0x220 / 4] = 0xF0;
    r.words[0x230 / 4] = 0x33;
    try std.testing.expectEqual(codes.ok, mg.clearStatus(&r, &log, 0, 0x0F, .{ 0x01, 0x10, 0x00 }));
    try std.testing.expectEqual([8]usize{ 0x208, 0x218, 0x228, 0x238, 0x200, 0x210, 0x220, 0x230 }, r.writes[0..8].*);
    try std.testing.expectEqual(@as(u32, 0x0F), r.at(off.meid));
    try std.testing.expectEqual(@as(u32, 0x10), r.at(0x228));
    try std.testing.expectEqual(@as(u32, 0xF0), r.at(off.meis));
    try std.testing.expectEqual(@as(u32, 0x0E), r.at(0x210));
    try std.testing.expectEqual(@as(u32, 0xE0), r.at(0x220));
    try std.testing.expectEqual(@as(u32, 0x33), r.at(0x230));
}

test "read_stats copies every counter from its register" {
    var r = Regs.init();
    var log: Log = .{};
    var s: mg.Stats = undefined;
    try std.testing.expectEqual(codes.ok, mg.readStats(&r, &log, 0, &s));
    try std.testing.expectEqual(r.at(0x300), s.pause_tx_manual);
    try std.testing.expectEqual(r.at(0x304), s.pause_tx_auto);
    try std.testing.expectEqual(r.at(0x308), s.pause_rx);
    try std.testing.expectEqual(r.at(0x30C), s.false_carrier);
    try std.testing.expectEqual(r.at(0x310), s.eee_count);
    try std.testing.expectEqual([2]u32{ r.at(0x320), r.at(0x324) }, s.pfc_tx_manual);
    try std.testing.expectEqual([2]u32{ r.at(0x330), r.at(0x334) }, s.pfc_tx_auto);
    try std.testing.expectEqual(r.at(0x340), s.pfc_rx[0]);
    try std.testing.expectEqual(r.at(0x35C), s.pfc_rx[7]);
    try std.testing.expectEqual(r.at(0x360), s.rx_overflow);
    try std.testing.expectEqual(r.at(0x364), s.rx_hdr_crc_err);
    try std.testing.expectEqual(r.at(0x408), s.rx[0]); // MRGFCE
    try std.testing.expectEqual(r.at(0x438), s.rx[12]); // MRFC
    try std.testing.expectEqual(r.at(0x458), s.rx[20]); // MRXBCPL
    try std.testing.expectEqual(r.at(0x508), s.tx[0]); // MTGFCE
    try std.testing.expectEqual(r.at(0x51C), s.tx[5]); // MTEFC
    try std.testing.expectEqual(r.at(0x52C), s.tx[9]); // MTXBCPL
    try std.testing.expectEqual(@as(usize, 0), r.nwrites);
}
