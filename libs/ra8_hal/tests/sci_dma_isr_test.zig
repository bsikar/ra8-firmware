//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/sci_dma_isr.zig.

const std = @import("std");
const sd = @import("sci_dma_isr");

/// One SCI channel's RDR/TDR/CCR0.
const Regs = struct {
    ccr0: u32 = sd.ccr0_tie | sd.ccr0_rie | 0x1,
    rdr: u32 = 0,
    tdr: [8]u8 = [_]u8{0} ** 8,
    tdr_writes: usize = 0,

    pub fn readCcr0(self: *Regs) u32 {
        return self.ccr0;
    }
    pub fn writeCcr0(self: *Regs, v: u32) void {
        self.ccr0 = v;
    }
    pub fn readRdr(self: *Regs) u32 {
        return self.rdr;
    }
    pub fn writeTdr(self: *Regs, b: u8) void {
        self.tdr[self.tdr_writes] = b;
        self.tdr_writes += 1;
    }
};

const Seen = struct { bytes: [8]u8 = [_]u8{0} ** 8, n: usize = 0, give: u8 = 0, more: bool = true };

fn onRx(ctx: ?*anyopaque, byte: u8) callconv(.C) void {
    const s: *Seen = @ptrCast(@alignCast(ctx.?));
    s.bytes[s.n] = byte;
    s.n += 1;
}

fn onTx(ctx: ?*anyopaque, byte: *u8) callconv(.C) bool {
    const s: *Seen = @ptrCast(@alignCast(ctx.?));
    s.bytes[s.n] = byte.*;
    s.n += 1;
    byte.* = s.give;
    return s.more;
}

test "makeRequest builds a byte-wide software-start request" {
    const r = sd.makeRequest(0x1000, 0x4035_8104, 7, true, false, null, null);
    try std.testing.expectEqual(@as(usize, 0x1000), r.src_addr);
    try std.testing.expectEqual(@as(usize, 0x4035_8104), r.dst_addr);
    try std.testing.expectEqual(@as(u16, 7), r.count);
    try std.testing.expectEqual(sd.dma_width_byte, r.width);
    try std.testing.expect(r.src_inc and !r.dst_inc);
    try std.testing.expectEqual(@as(u16, 0), r.trigger);
}

test "argsOk rejects a zero length and a channel past SCI9" {
    try std.testing.expect(sd.argsOk(0, 1));
    try std.testing.expect(sd.argsOk(9, 1));
    try std.testing.expect(!sd.argsOk(10, 1));
    try std.testing.expect(!sd.argsOk(3, 0));
}

test "TXI streams the async buffer and clears TIE when drained" {
    var regs = Regs{};
    const buf = [_]u8{ 0xA1, 0xB2 };
    var seen = Seen{};
    var st = sd.State{ .tx_buf = &buf, .tx_len = 2, .tx_fn = onTx, .tx_ctx = &seen };
    sd.dispatchTxi(&regs, &st);
    try std.testing.expectEqual(@as(u32, 1), st.tx_idx);
    try std.testing.expect(regs.ccr0 & sd.ccr0_tie != 0);
    sd.dispatchTxi(&regs, &st);
    try std.testing.expectEqualSlices(u8, &buf, regs.tdr[0..2]);
    try std.testing.expectEqualSlices(u8, &buf, seen.bytes[0..2]);
    try std.testing.expectEqual(@as(u32, 0), regs.ccr0 & sd.ccr0_tie);
    try std.testing.expect(st.tx_buf == null and st.tx_len == 0 and st.tx_idx == 0);
}

test "TXI with no handler and nothing queued clears TIE" {
    var regs = Regs{};
    var st = sd.State{};
    sd.dispatchTxi(&regs, &st);
    try std.testing.expectEqual(@as(u32, 0), regs.ccr0 & sd.ccr0_tie);
    try std.testing.expectEqual(@as(u32, 0x1 | sd.ccr0_rie), regs.ccr0);
    try std.testing.expectEqual(@as(usize, 0), regs.tdr_writes);
}

test "TXI handler path writes its byte or stops TIE" {
    var regs = Regs{};
    var seen = Seen{ .give = 0x5A };
    var st = sd.State{ .tx_fn = onTx, .tx_ctx = &seen };
    sd.dispatchTxi(&regs, &st);
    try std.testing.expectEqual(@as(u8, 0x5A), regs.tdr[0]);
    try std.testing.expect(regs.ccr0 & sd.ccr0_tie != 0);
    seen.more = false;
    sd.dispatchTxi(&regs, &st);
    try std.testing.expectEqual(@as(usize, 1), regs.tdr_writes);
    try std.testing.expectEqual(@as(u32, 0), regs.ccr0 & sd.ccr0_tie);
}

test "RXI fills the async buffer, clears RIE when full and still calls the handler" {
    var regs = Regs{ .rdr = 0x1_37 };
    var buf = [_]u8{0} ** 2;
    var seen = Seen{};
    var st = sd.State{ .rx_buf = &buf, .rx_len = 2, .rx_fn = onRx, .rx_ctx = &seen };
    sd.dispatchRxi(&regs, &st);
    try std.testing.expect(regs.ccr0 & sd.ccr0_rie != 0);
    regs.rdr = 0x42;
    sd.dispatchRxi(&regs, &st);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x37, 0x42 }, &buf);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x37, 0x42 }, seen.bytes[0..2]);
    try std.testing.expectEqual(@as(u32, 0), regs.ccr0 & sd.ccr0_rie);
    try std.testing.expect(regs.ccr0 & sd.ccr0_tie != 0);
    try std.testing.expect(st.rx_buf == null and st.rx_len == 0 and st.rx_idx == 0);
}

test "RXI with nothing queued only feeds the handler" {
    var regs = Regs{ .rdr = 0x99 };
    var seen = Seen{};
    var st = sd.State{ .rx_fn = onRx, .rx_ctx = &seen };
    sd.dispatchRxi(&regs, &st);
    try std.testing.expectEqual(@as(usize, 1), seen.n);
    try std.testing.expectEqual(@as(u8, 0x99), seen.bytes[0]);
    try std.testing.expect(regs.ccr0 & sd.ccr0_rie != 0);
}

test "State and DmaRequest match the C layouts on host" {
    try std.testing.expectEqual(@as(usize, 72), @sizeOf(sd.State));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(sd.State, "initialized"));
    try std.testing.expectEqual(@as(usize, 40), @sizeOf(sd.DmaRequest));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(sd.DmaRequest, "on_complete"));
}
