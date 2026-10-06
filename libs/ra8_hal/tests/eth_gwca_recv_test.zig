//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_recv.zig (RA8FW-857).

const std = @import("std");
const r = @import("eth_gwca_recv");
const q = r.q;

/// rxFrame returns a canned code; reloadQueue records the queue index.
const Hw = struct {
    code: u16,
    reloaded: *?u32,
    pub fn rxFrame(self: Hw, _: ?[*]volatile q.Desc, _: u32, _: *u32, _: ?[*]u8, _: u32, _: u32, _: ?*u32) u16 {
        return self.code;
    }
    pub fn reloadQueue(self: Hw, qi: u32) u16 {
        self.reloaded.* = qi;
        return q.ok;
    }
    pub fn nullPtr(_: Hw, _: [*:0]const u8) u16 {
        return q.null_ptr;
    }
};

/// Descriptor pointers are 40-bit, so the ring is a file-scope global
/// (the image's data section sits below 1 TiB); stack addresses are not.
var ring: [4]q.Desc = undefined;

fn freshRing() ![]q.Desc {
    if (@intFromPtr(&ring) >= (@as(usize, 1) << 40)) return error.SkipZigTest;
    ring = @splat(.{});
    return &ring;
}

fn state(chain: []q.Desc) r.DefaultState {
    var s = std.mem.zeroes(r.DefaultState);
    s.rx_chain = chain.ptr;
    s.rx_depth = @intCast(chain.len);
    s.rx_queue_index = 3;
    s.rx_head = 2;
    return s;
}

test "no_data with a LEMPTY terminator relinks to chain[0], resets the head and reloads" {
    const chain = try freshRing();
    q.setDt(&chain[3], r.dt_lempty);
    var s = state(chain);
    var reloaded: ?u32 = null;
    try std.testing.expectEqual(q.no_data, r.recv(Hw{ .code = q.no_data, .reloaded = &reloaded }, &s, null, 0, null));
    try std.testing.expectEqual(r.dt_link, q.getDt(&chain[3]));
    try std.testing.expectEqual(@as(?[*]u8, @ptrCast(&chain[0])), q.decodePtr(&chain[3]));
    try std.testing.expectEqual(@as(u32, 0), s.rx_head);
    try std.testing.expectEqual(@as(?u32, 3), reloaded);
}

test "a live terminator or a received frame leaves the ring alone" {
    const chain = try freshRing();
    q.setDt(&chain[3], r.dt_link);
    var s = state(chain);
    var reloaded: ?u32 = null;
    try std.testing.expectEqual(q.no_data, r.recv(Hw{ .code = q.no_data, .reloaded = &reloaded }, &s, null, 0, null));
    q.setDt(&chain[3], r.dt_lempty);
    try std.testing.expectEqual(q.ok, r.recv(Hw{ .code = q.ok, .reloaded = &reloaded }, &s, null, 0, null));
    try std.testing.expectEqual(@as(?u32, null), reloaded);
    try std.testing.expectEqual(@as(u32, 2), s.rx_head);
}

test "null state is rejected" {
    var reloaded: ?u32 = null;
    try std.testing.expectEqual(q.null_ptr, r.recv(Hw{ .code = q.ok, .reloaded = &reloaded }, null, null, 0, null));
}
