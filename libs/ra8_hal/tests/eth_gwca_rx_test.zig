//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_rx.zig (RA8FW-856).

const std = @import("std");
const rx = @import("eth_gwca_rx");
const q = rx.q;

/// findSlot is the real queue search; nullPtr records the message.
const Hw = struct {
    last: *?[]const u8,
    pub fn findSlot(self: Hw, chain: [*]volatile q.Desc, depth: u32, dt: u8, start: u32, out: *u32) u16 {
        return q.findSlot(self, chain, depth, dt, start, out);
    }
    pub fn nullPtr(self: Hw, msg: [*:0]const u8) u16 {
        self.last.* = std.mem.span(msg);
        return q.null_ptr;
    }
};

/// Descriptor pointers are 40-bit (ptr_h:ptr_l), so frame buffers live in
/// the image's data section, which sits below 1 TiB on Linux, macOS and
/// Windows. Stack and heap addresses can be above it.
var bufs: [3][16]u8 = undefined;

fn lowOrSkip() !void {
    if (@intFromPtr(&bufs) >= (@as(usize, 1) << 40)) return error.SkipZigTest;
}

fn point(d: *q.Desc, buf: []u8) void {
    const addr: u64 = @intFromPtr(buf.ptr);
    d.ptr_h = @truncate(addr >> 32);
    d.ptr_l = @truncate(addr);
}

test "rxFrame copies the FSINGLE slot, re-arms it FEMPTY and advances the head" {
    try lowOrSkip();
    var last: ?[]const u8 = null;
    var chain = [_]q.Desc{.{}} ** 4;
    for (0..3) |i| {
        point(&chain[i], &bufs[i]);
        q.setDt(&chain[i], q.dt_fempty);
    }
    @memcpy(bufs[1][0..5], "hello");
    q.setDs(&chain[1], 5);
    q.setDt(&chain[1], q.dt_fsingle);
    var head: u32 = 0;
    var out: [16]u8 = undefined;
    var len: u32 = 0;
    try std.testing.expectEqual(q.ok, rx.rxFrame(Hw{ .last = &last }, &chain, 4, &head, &out, out.len, 16, &len));
    try std.testing.expectEqualStrings("hello", out[0..len]);
    try std.testing.expectEqual(@as(u32, 2), head);
    try std.testing.expectEqual(q.dt_fempty, q.getDt(&chain[1]));
    try std.testing.expectEqual(@as(u32, 16), q.getDs(&chain[1]));
    try std.testing.expectEqual(q.no_data, rx.rxFrame(Hw{ .last = &last }, &chain, 4, &head, &out, out.len, 16, &len));
}

test "rxFrame rejects a frame larger than the caller's buffer and leaves the slot" {
    try lowOrSkip();
    var last: ?[]const u8 = null;
    var chain = [_]q.Desc{.{}} ** 2;
    point(&chain[0], &bufs[0]);
    q.setDs(&chain[0], 12);
    q.setDt(&chain[0], q.dt_fsingle);
    var head: u32 = 0;
    var out: [8]u8 = undefined;
    var len: u32 = 0;
    try std.testing.expectEqual(q.invalid_arg, rx.rxFrame(Hw{ .last = &last }, &chain, 2, &head, &out, out.len, 16, &len));
    try std.testing.expectEqual(q.dt_fsingle, q.getDt(&chain[0]));
    try std.testing.expectEqual(@as(u32, 0), head);
}

test "rxFrame argument checks" {
    var last: ?[]const u8 = null;
    var chain = [_]q.Desc{.{}} ** 4;
    var head: u32 = 0;
    var out: [8]u8 = undefined;
    var len: u32 = 0;
    const hw = Hw{ .last = &last };
    try std.testing.expectEqual(q.null_ptr, rx.rxFrame(hw, null, 4, &head, &out, 8, 16, &len));
    try std.testing.expectEqualStrings("rx_frame: chain null", last.?);
    try std.testing.expectEqual(q.null_ptr, rx.rxFrame(hw, &chain, 4, &head, &out, 8, 16, null));
    try std.testing.expectEqualStrings("rx_frame: out_len null", last.?);
    try std.testing.expectEqual(q.invalid_arg, rx.rxFrame(hw, &chain, 4, &head, &out, 0, 16, &len));
    try std.testing.expectEqual(q.invalid_arg, rx.rxFrame(hw, &chain, 1, &head, &out, 8, 16, &len));
}
