//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_capture_bind` around a scripted inner transport: the bound
//! rows forward, and every frame and HANDSHAKE edge reaches the sink as text.

const std = @import("std");
const capture = @import("capture_abi");

const c = capture.c;
const frame_bytes: u16 = 1600;

const Inner = struct {
    levels: [3]bool = .{ false, true, true },
    sampled: usize = 0,
    verdict: c.ra8_err_t = 0,
    delayed: u32 = 0,
    reply: [4]u8 = .{ 0xDE, 0xAD, 0x00, 0x01 },
};

fn innerTransfer(ctx: ?*anyopaque, tx: [*c]const u8, rx: [*c]u8, len: u16) callconv(.c) c.ra8_err_t {
    const inner: *Inner = @ptrCast(@alignCast(ctx.?));
    _ = tx;
    @memset(rx[0..len], 0);
    @memcpy(rx[0..inner.reply.len], &inner.reply);
    return inner.verdict;
}

fn innerHandshake(ctx: ?*anyopaque) callconv(.c) bool {
    const inner: *Inner = @ptrCast(@alignCast(ctx.?));
    const level = inner.levels[@min(inner.sampled, inner.levels.len - 1)];
    inner.sampled += 1;
    return level;
}

fn innerDelay(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    const inner: *Inner = @ptrCast(@alignCast(ctx.?));
    inner.delayed += ms;
}

const Text = struct {
    buf: [8192]u8 = undefined,
    len: usize = 0,
    pieces: usize = 0,

    fn slice(self: *const Text) []const u8 {
        return self.buf[0..self.len];
    }
};

fn sink(ctx: ?*anyopaque, text: [*c]const u8, len: u16) callconv(.c) void {
    const out: *Text = @ptrCast(@alignCast(ctx.?));
    @memcpy(out.buf[out.len..][0..len], text[0..len]);
    out.len += len;
    out.pieces += 1;
}

fn innerRows(inner: *Inner) c.ra8_c6link_transport_t {
    return .{ .transfer = innerTransfer, .handshake_active = innerHandshake, .delay_ms = innerDelay, .ctx = inner };
}

test "bound rows forward and report edges, tx and rx with trailing zeros dropped" {
    var inner: Inner = .{};
    var text: Text = .{};
    var cap: c.ra8_c6link_capture_t = undefined;
    var bound: c.ra8_c6link_transport_t = undefined;
    const rows = innerRows(&inner);
    try std.testing.expectEqual(@as(c.ra8_err_t, 0), capture.ra8_c6link_capture_bind(&cap, &rows, sink, &text, &bound));

    var tx = [_]u8{0} ** frame_bytes;
    tx[0] = 0x02;
    tx[1] = 0x00;
    tx[2] = 0x10;
    var rx: [frame_bytes]u8 = undefined;
    try std.testing.expect(!bound.handshake_active.?(bound.ctx));
    try std.testing.expect(bound.handshake_active.?(bound.ctx));
    try std.testing.expect(bound.handshake_active.?(bound.ctx));
    try std.testing.expectEqual(@as(c.ra8_err_t, 0), bound.transfer.?(bound.ctx, &tx, &rx, frame_bytes));
    bound.delay_ms.?(bound.ctx, 7);

    try std.testing.expectEqualStrings(
        "c6cap 0 hs 0\nc6cap 0 hs 1\nc6cap 0 tx 020010\nc6cap 0 rx dead0001\n",
        text.slice(),
    );
    try std.testing.expectEqual(@as(u8, 0xDE), rx[0]);
    try std.testing.expectEqual(@as(u32, 1), cap.seq);
    try std.testing.expectEqual(@as(u32, 7), inner.delayed);
}

test "an all-zero frame has no hex, a failed transfer logs tx only, and long frames are chunked" {
    var inner: Inner = .{ .verdict = 0x402 };
    var text: Text = .{};
    var cap: c.ra8_c6link_capture_t = undefined;
    var bound: c.ra8_c6link_transport_t = undefined;
    const rows = innerRows(&inner);
    try std.testing.expectEqual(@as(c.ra8_err_t, 0), capture.ra8_c6link_capture_bind(&cap, &rows, sink, &text, &bound));

    var tx = [_]u8{0} ** frame_bytes;
    var rx: [frame_bytes]u8 = undefined;
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x402), bound.transfer.?(bound.ctx, &tx, &rx, frame_bytes));
    try std.testing.expectEqualStrings("c6cap 0 tx\n", text.slice());

    text = .{};
    @memset(tx[0..130], 0xAB);
    _ = bound.transfer.?(bound.ctx, &tx, &rx, frame_bytes);
    const line = text.slice();
    try std.testing.expect(std.mem.startsWith(u8, line, "c6cap 1 tx abab"));
    try std.testing.expectEqual(@as(usize, "c6cap 1 tx ".len + 260 + 1), line.len);
    // head, space, three hex chunks (64 + 64 + 2 bytes), newline
    try std.testing.expectEqual(@as(usize, 6), text.pieces);
}

test "bind refuses null pointers, a missing row and a missing sink" {
    var inner: Inner = .{};
    var text: Text = .{};
    var cap: c.ra8_c6link_capture_t = undefined;
    var bound: c.ra8_c6link_transport_t = undefined;
    var rows = innerRows(&inner);
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x504), capture.ra8_c6link_capture_bind(null, &rows, sink, &text, &bound));
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x504), capture.ra8_c6link_capture_bind(&cap, null, sink, &text, &bound));
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x504), capture.ra8_c6link_capture_bind(&cap, &rows, sink, &text, null));
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x103), capture.ra8_c6link_capture_bind(&cap, &rows, null, &text, &bound));
    rows.delay_ms = null;
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x103), capture.ra8_c6link_capture_bind(&cap, &rows, sink, &text, &bound));
}
