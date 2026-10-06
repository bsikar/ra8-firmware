//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_frame.zig.

const std = @import("std");
const cf = @import("canfd_frame");

/// One channel's register file as a byte array; `done_after` sets TMTRF
/// once TMSTS0 has been read that many times (0 = never).
const Regs = struct {
    mem: [0x700]u8 = @splat(0),
    order: [8]usize = undefined,
    n: usize = 0,
    tm_reads: u32 = 0,
    done_after: u32 = 0,

    fn note(self: *Regs, off: usize) void {
        if (self.n < self.order.len) self.order[self.n] = off;
        self.n += 1;
    }
    pub fn read8(self: *Regs, off: usize) u8 {
        if (off == cf.off_tmsts0) {
            self.tm_reads += 1;
            if (self.done_after != 0 and self.tm_reads >= self.done_after) return cf.tmtrf_done;
        }
        return self.mem[off];
    }
    pub fn write8(self: *Regs, off: usize, value: u8) void {
        if (off < cf.off_tm0 + cf.off_df) self.note(off);
        self.mem[off] = value;
    }
    pub fn read32(self: *Regs, off: usize) u32 {
        return std.mem.readInt(u32, self.mem[off..][0..4], .little);
    }
    pub fn write32(self: *Regs, off: usize, value: u32) void {
        self.note(off);
        std.mem.writeInt(u32, self.mem[off..][0..4], value, .little);
    }
};

fn frameOf(id: u32, dlc: u8, ext: u8, fd: u8, brs: u8) cf.Frame {
    var f = cf.Frame{ .id = id, .dlc = dlc, .is_extended = ext, .is_fd = fd, .is_brs = brs, .data = undefined };
    for (&f.data, 0..) |*b, i| b.* = @intCast(i + 1);
    return f;
}

test "the frame struct matches ra8_canfd_frame_t" {
    try std.testing.expectEqual(@as(usize, 72), @sizeOf(cf.Frame));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(cf.Frame, "dlc"));
    try std.testing.expectEqual(@as(usize, 7), @offsetOf(cf.Frame, "is_brs"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(cf.Frame, "data"));
}

test "validate rejects a long DLC, an oversized ID and BRS without FD" {
    try std.testing.expectEqual(cf.invalid_arg, cf.validate(&frameOf(1, 16, 0, 1, 0)));
    try std.testing.expectEqual(cf.invalid_arg, cf.validate(&frameOf(0x800, 8, 0, 0, 0)));
    try std.testing.expectEqual(cf.ok, cf.validate(&frameOf(0x800, 8, 1, 0, 0)));
    try std.testing.expectEqual(cf.invalid_arg, cf.validate(&frameOf(0x2000_0000, 8, 1, 0, 0)));
    try std.testing.expectEqual(cf.invalid_arg, cf.validate(&frameOf(1, 8, 0, 0, 1)));
    try std.testing.expectEqual(cf.ok, cf.validate(&frameOf(0x7FF, 15, 0, 1, 1)));
}

test "TX ID and FDCTR words follow IDE, FDF and BRS" {
    try std.testing.expectEqual(@as(u32, 0x123), cf.txId(&frameOf(0x123, 0, 0, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0x8000_1234), cf.txId(&frameOf(0x1234, 0, 1, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0), cf.txFdctr(&frameOf(0, 0, 0, 0, 0)));
    try std.testing.expectEqual(@as(u32, 0x6), cf.txFdctr(&frameOf(0, 0, 0, 1, 1)));
}

test "transmit clears TMTRF, loads MB 0, sets TXREQ and waits" {
    var r = Regs{ .done_after = 3 };
    r.mem[cf.off_tmsts0] = 0x06;
    const f = frameOf(0x1ABCDEF, 15, 1, 1, 1);
    try std.testing.expectEqual(cf.ok, cf.transmit(&r, &f));
    const want = [_]usize{ cf.off_tmsts0, cf.off_tm0, cf.off_tm0 + 4, cf.off_tm0 + 8, cf.off_tmc0 };
    try std.testing.expectEqualSlices(usize, &want, r.order[0..5]);
    try std.testing.expectEqual(@as(u32, 0x81AB_CDEF), r.read32(cf.off_tm0));
    try std.testing.expectEqual(@as(u32, 0xF000_0000), r.read32(cf.off_tm0 + 4));
    try std.testing.expectEqual(@as(u32, 0x6), r.read32(cf.off_tm0 + 8));
    try std.testing.expectEqualSlices(u8, &f.data, r.mem[cf.off_tm0 + 12 ..][0..64]);
    try std.testing.expectEqual(cf.tmc_txreq, r.mem[cf.off_tmc0]);
    try std.testing.expectEqual(@as(u32, 3), r.tm_reads);
}

test "an invalid frame touches no register" {
    var r = Regs{};
    try std.testing.expectEqual(cf.invalid_arg, cf.transmit(&r, &frameOf(1, 9, 0, 0, 1)));
    try std.testing.expectEqual(@as(usize, 0), r.n);
}

test "the TX wait gives up after the poll budget without an error" {
    var r = Regs{};
    try std.testing.expectEqual(cf.tx_spin, cf.waitTxComplete(&r));
    try std.testing.expectEqual(cf.tx_spin, r.tm_reads);
    var r2 = Regs{ .done_after = 1 };
    try std.testing.expectEqual(@as(u32, 1), cf.waitTxComplete(&r2));
}

test "receive reports an empty FIFO without popping it" {
    var r = Regs{};
    r.write32(cf.off_rfsts0, cf.rfsts_empty);
    r.n = 0;
    var out = frameOf(0, 0, 0, 0, 0);
    try std.testing.expectEqual(cf.no_data, cf.receive(&r, &out));
    try std.testing.expectEqual(@as(usize, 0), r.n);
}

test "receive decodes FIFO 0 and pops it with 0xFF" {
    var r = Regs{};
    r.write32(cf.off_rf0, 0x8000_0042);
    r.write32(cf.off_rf0 + 4, 0x9000_0000);
    r.write32(cf.off_rf0 + 8, cf.fd_fdf | cf.fd_brs);
    for (0..64) |i| r.mem[cf.off_rf0 + 12 + i] = @intCast(200 - i);
    var out = frameOf(0, 0, 0, 0, 0);
    try std.testing.expectEqual(cf.ok, cf.receive(&r, &out));
    try std.testing.expectEqual(@as(u32, 0x42), out.id);
    try std.testing.expectEqual(@as(u8, 1), out.is_extended);
    try std.testing.expectEqual(@as(u8, 9), out.dlc);
    try std.testing.expectEqual(@as(u8, 1), out.is_fd);
    try std.testing.expectEqual(@as(u8, 1), out.is_brs);
    try std.testing.expectEqual(@as(u8, 200), out.data[0]);
    try std.testing.expectEqual(@as(u8, 137), out.data[63]);
    try std.testing.expectEqual(cf.rfpctr_ack, r.read32(cf.off_rfpctr0));
}

test "a standard RX ID drops the bits above 11" {
    var out = frameOf(0, 0, 0, 0, 0);
    cf.decodeHeader(0x0000_FFFF, 0, 0, &out);
    try std.testing.expectEqual(@as(u32, 0x7FF), out.id);
    try std.testing.expectEqual(@as(u8, 0), out.is_extended);
    try std.testing.expectEqual(@as(u8, 0), out.is_fd);
}

test "error counters come from CFDC0.STS" {
    var r = Regs{};
    r.write32(cf.off_sts, 0xA55A_0000);
    const c = cf.errorCounters(&r);
    try std.testing.expectEqual(@as(u8, 0xA5), c.tec);
    try std.testing.expectEqual(@as(u8, 0x5A), c.rec);
}
