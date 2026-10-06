//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_send.zig (RA8FW-858).

const std = @import("std");
const t = @import("eth_gwca_send");
const q = t.q;

/// Records the call order; txDone reports done from iteration `done_at`.
const Hw = struct {
    log: *std.ArrayList(u8),
    done_at: u32,
    kick: u16 = q.ok,
    pub fn dsb(self: Hw) void {
        self.log.append('d') catch unreachable;
    }
    pub fn reloadQueue(self: Hw, _: u32) u16 {
        self.log.append('r') catch unreachable;
        return q.ok;
    }
    pub fn kickTx(self: Hw, _: u32) u16 {
        self.log.append('k') catch unreachable;
        return self.kick;
    }
    pub fn txDone(self: Hw, _: *volatile t.ExtDesc, iter: u32) bool {
        return iter >= self.done_at;
    }
    pub fn nullPtr(_: Hw, _: [*:0]const u8) u16 {
        return q.null_ptr;
    }
    pub fn logError(self: Hw, _: [*:0]const u8) void {
        self.log.append('e') catch unreachable;
    }
};

/// Descriptor pointers are 40-bit, so the ring is a file-scope global.
var ring: [4]t.ExtDesc = undefined;
var pool: [64]u8 = undefined;

fn freshState() !t.DefaultState {
    if (@intFromPtr(&ring) >= (@as(usize, 1) << 40)) return error.SkipZigTest;
    ring = [_]t.ExtDesc{.{}} ** 4;
    var s = std.mem.zeroes(t.DefaultState);
    s.tx_chain = &ring;
    s.tx_depth = 4;
    s.tx_pool = &pool;
    s.tx_slot_bytes = pool.len;
    s.tx_queue_index = 2;
    s.tx_tail = 3;
    s.mac_port = 1;
    return s;
}

test "send fills slot 0 and kicks after the barrier" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var s = try freshState();
    const frame = [_]u8{ 1, 2, 3, 4, 5 };
    try std.testing.expectEqual(q.ok, t.send(Hw{ .log = &log, .done_at = 3 }, &s, &frame, frame.len));
    try std.testing.expectEqualStrings("drk", log.items);
    try std.testing.expectEqualSlices(u8, &frame, pool[0..5]);
    try std.testing.expectEqual(@as(u32, 5), q.getDs(&ring[0].base));
    try std.testing.expectEqual(t.dt_fsingle, q.getDt(&ring[0].base));
    try std.testing.expectEqual(@as(u32, 4), ring[0].info1_lo);
    try std.testing.expectEqual(@as(u32, 0x20000), ring[0].info1_hi);
    try std.testing.expectEqual(@as(u32, 0), s.tx_tail);
}

test "send rejects bad args and times out" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var s = try freshState();
    const frame = [_]u8{0} ** 65;
    const hw = Hw{ .log = &log, .done_at = std.math.maxInt(u32) };
    try std.testing.expectEqual(q.null_ptr, t.send(hw, null, &frame, 1));
    try std.testing.expectEqual(q.null_ptr, t.send(hw, &s, null, 1));
    try std.testing.expectEqual(q.invalid_arg, t.send(hw, &s, &frame, 0));
    try std.testing.expectEqual(q.invalid_arg, t.send(hw, &s, &frame, 65));
    try std.testing.expectEqual(t.hw_timeout, t.send(hw, &s, &frame, 8));
    try std.testing.expectEqualStrings("drke", log.items);
}

test "send re-arms an idle ring and info1Hi stays in DV" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var s = try freshState();
    q.setDt(&ring[3].base, 12);
    const frame = [_]u8{9};
    try std.testing.expectEqual(q.ok, t.send(Hw{ .log = &log, .done_at = 0 }, &s, &frame, 1));
    try std.testing.expectEqualStrings("rdrk", log.items);
    try std.testing.expectEqual(@as(u8, 14), q.getDt(&ring[3].base));
    try std.testing.expectEqual(@intFromPtr(&ring[0]), @intFromPtr(q.decodePtr(&ring[3].base).?));
    try std.testing.expectEqual(@as(u32, 0x400000), t.info1Hi(6));
    try std.testing.expectEqual(@as(u32, 0), t.info1Hi(7));
    try std.testing.expectEqual(@as(u32, 0), t.info1Hi(40));
}
