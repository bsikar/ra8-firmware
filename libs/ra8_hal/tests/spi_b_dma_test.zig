//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_dma.zig.

const std = @import("std");
const dma = @import("spi_b_dma");

test "Request mirrors ra8_dma_request_t field order" {
    const p = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 2 * p), @offsetOf(dma.Request, "count"));
    try std.testing.expectEqual(@as(usize, 2 * p + 2), @offsetOf(dma.Request, "width"));
    try std.testing.expectEqual(@as(usize, 2 * p + 5), @offsetOf(dma.Request, "trigger"));
    try std.testing.expectEqual(std.mem.alignForward(usize, 2 * p + 6, p), @offsetOf(dma.Request, "on_complete"));
}

test "argsOk rejects a bad channel and a zero length" {
    try std.testing.expect(dma.argsOk(0, 1) and dma.argsOk(1, 0xFFFF));
    try std.testing.expect(!dma.argsOk(2, 1));
    try std.testing.expect(!dma.argsOk(0, 0));
}

test "spdrAddr steps by the channel stride" {
    try std.testing.expectEqual(@as(usize, 0x4035C000), dma.spdrAddr(0));
    try std.testing.expectEqual(@as(usize, 0x4035C100), dma.spdrAddr(1));
}

test "roundUp to whole cache lines" {
    try std.testing.expectEqual(@as(u32, 0), dma.roundUp(0, 32));
    try std.testing.expectEqual(@as(u32, 32), dma.roundUp(1, 32));
    try std.testing.expectEqual(@as(u32, 64), dma.roundUp(33, 32));
    try std.testing.expectEqual(@as(u32, 7), dma.roundUp(7, 0));
}

fn noop(_: ?*anyopaque) callconv(.c) void {}

test "tx and rx requests point the right way" {
    var byte: u8 = 0;
    const tx = dma.txRequest(1, 0x2000, 16, noop, &byte);
    try std.testing.expectEqual(@as(usize, 0x2000), tx.src_addr);
    try std.testing.expectEqual(@as(usize, 0x4035C100), tx.dst_addr);
    try std.testing.expect(tx.src_inc and !tx.dst_inc);
    try std.testing.expectEqual(@as(u16, 16), tx.count);
    const rx = dma.rxRequest(0, 0x3000, 8, null, null);
    try std.testing.expectEqual(@as(usize, 0x4035C000), rx.src_addr);
    try std.testing.expectEqual(@as(usize, 0x3000), rx.dst_addr);
    try std.testing.expect(!rx.src_inc and rx.dst_inc);
    try std.testing.expectEqual(dma.width_byte, rx.width);
    try std.testing.expectEqual(@as(u8, 0), rx.trigger);
}
