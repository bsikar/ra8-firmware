//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i2c_xfer.zig (RA8FW-887).

const std = @import("std");
const xfer = @import("i2c_xfer");

const off_iccr2: usize = 0x01;
const off_icmr3: usize = 0x04;
const off_icsr2: usize = 0x09;
const off_icdrt: usize = 0x12;
const tdre: u8 = 1 << 7;
const nackf: u8 = 1 << 4;
const sp: u8 = 1 << 3;

/// Byte register file. ICSR2 reads `icsr2`; ICDRT writes are logged;
/// ICDRR reads walk `rx`; `stuck` makes every poll fail; `nack_on_tx`
/// latches NACKF when a byte is written, as a target that does not answer.
const Regs = struct {
    mem: [0x16]u8 = @splat(0),
    icsr2: u8 = tdre | xfer.icsr2_tend | xfer.icsr2_rdrf,
    tx: [8]u8 = @splat(0),
    tx_len: usize = 0,
    rx: []const u8 = &.{},
    rx_pos: usize = 0,
    stuck: bool = false,
    nack_on_tx: bool = false,

    pub fn read8(self: *Regs, off: usize) u8 {
        if (off == off_icsr2) return self.icsr2;
        if (off == xfer.off_icdrr) {
            // The dummy read returns 0 and does not consume rx.
            if (self.rx_pos == 0 and self.mem[0x15] == 0) {
                self.mem[0x15] = 1;
                return 0;
            }
            const v = self.rx[self.rx_pos];
            self.rx_pos += 1;
            return v;
        }
        return self.mem[off];
    }
    pub fn write8(self: *Regs, off: usize, v: u8) void {
        if (off == off_icsr2) {
            self.icsr2 = v;
            return;
        }
        if (off == off_icdrt) {
            self.tx[self.tx_len] = v;
            self.tx_len += 1;
            if (self.nack_on_tx) self.icsr2 |= nackf;
        }
        self.mem[off] = v;
    }
    pub fn poll(self: *Regs, _: usize, _: u32, cond: bool) bool {
        return !self.stuck and cond;
    }
};

test "address byte shifts and sets R/W" {
    try std.testing.expectEqual(@as(u8, 0xA0), xfer.addressByte(0x50, false));
    try std.testing.expectEqual(@as(u8, 0xA1), xfer.addressByte(0x50, true));
}

test "write sends address then data and stops" {
    var r = Regs{};
    var held = false;
    const rc = xfer.write(&r, &held, 0x50, &.{ 0x11, 0x22 }, true);
    try std.testing.expectEqual(@as(u16, 0), rc);
    try std.testing.expectEqualSlices(u8, &.{ 0xA0, 0x11, 0x22 }, r.tx[0..r.tx_len]);
    try std.testing.expect(!held);
    try std.testing.expect(r.mem[off_iccr2] & sp != 0);
}

test "write without stop holds the bus" {
    var r = Regs{};
    var held = false;
    try std.testing.expectEqual(@as(u16, 0), xfer.write(&r, &held, 0x50, &.{0x01}, false));
    try std.testing.expect(held);
    try std.testing.expect(r.mem[off_iccr2] & sp == 0);
}

test "data NACK aborts the write and releases the bus" {
    var r = Regs{};
    var held = true;
    try std.testing.expectEqual(@as(u16, 0), xfer.drainTx(&r, &.{}));
    r.icsr2 = tdre | nackf;
    try std.testing.expectEqual(@as(u16, 0x407), xfer.drainTx(&r, &.{0x01}));
    try std.testing.expectEqual(@as(u16, 0x407), xfer.finishTx(&r, 0x407, false, &held));
    try std.testing.expect(!held);
}

test "read returns bytes and arms WAIT and NACK for a short read" {
    var r = Regs{ .rx = &.{ 0xDE, 0xAD } };
    var held = false;
    var out: [2]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), xfer.read(&r, &held, 0x50, &out));
    try std.testing.expectEqualSlices(u8, &.{ 0xDE, 0xAD }, &out);
    try std.testing.expectEqual(@as(u8, 0xA1), r.tx[0]);
    // WAIT and ACKBT are cleared again at the end.
    try std.testing.expectEqual(@as(u8, 0), r.mem[off_icmr3] & (xfer.icmr3_wait | (1 << 3)));
    try std.testing.expect(r.mem[off_iccr2] & sp != 0);
}

test "read times out when RDRF never sets" {
    var r = Regs{ .icsr2 = tdre };
    var out: [1]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0x203), xfer.drainRx(&r, &out));
}

test "scan reports ACK and NACK without failing" {
    var r = Regs{};
    var held = false;
    var acked = false;
    try std.testing.expectEqual(@as(u16, 0), xfer.scan(&r, &held, 0x3C, &acked));
    try std.testing.expect(acked);
    r = Regs{ .icsr2 = tdre, .nack_on_tx = true };
    try std.testing.expectEqual(@as(u16, 0), xfer.scan(&r, &held, 0x3C, &acked));
    try std.testing.expect(!acked);
}

test "address NACK on write stops and releases the bus" {
    var r = Regs{ .icsr2 = tdre, .nack_on_tx = true };
    var held = true;
    try std.testing.expectEqual(@as(u16, 0x407), xfer.write(&r, &held, 0x50, &.{0x01}, false));
    try std.testing.expect(!held);
    try std.testing.expectEqual(@as(usize, 1), r.tx_len);
}

test "busy bus is refused unless held" {
    var r = Regs{};
    r.mem[off_iccr2] = 1 << 7;
    var held = false;
    var acked = true;
    try std.testing.expectEqual(@as(u16, 0x109), xfer.scan(&r, &held, 0x3C, &acked));
    try std.testing.expect(!acked);
}
