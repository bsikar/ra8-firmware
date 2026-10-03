//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The poll pump against a scripted port: handshake give-up, filler and
//! staged frames, integrity classes, transport faults and early stop.

const std = @import("std");
const implementation = @import("implementation");

const pump = implementation.pump;
const frame = implementation.frame;
const Frame = frame.Frame;

/// A scripted co-processor: a handshake pattern, one canned reply frame and a
/// transport that can be told to fail.
const Model = struct {
    tx_buf: [Frame.bytes]u8 = [_]u8{0xAA} ** Frame.bytes,
    rx_buf: [Frame.bytes]u8 = [_]u8{0} ** Frame.bytes,
    reply: [Frame.bytes]u8 = [_]u8{0} ** Frame.bytes,
    armed: bool = true,
    miss_pattern: []const bool = &.{},
    samples: usize = 0,
    slept_ms: u32 = 0,
    staged: ?pump.Staged = null,
    fail_transfer: bool = false,
    sent_len: [8]u16 = [_]u16{0} ** 8,
    sent: usize = 0,
    dispatched: usize = 0,
    stop_on_dispatch: bool = true,

    pub fn handshakeActive(self: *Model) bool {
        defer self.samples += 1;
        if (self.samples < self.miss_pattern.len) return self.miss_pattern[self.samples];
        return self.armed;
    }
    pub fn delayMs(self: *Model, ms: u16) void {
        self.slept_ms += ms;
    }
    pub fn tx(self: *Model) []u8 {
        return &self.tx_buf;
    }
    pub fn rx(self: *Model) []const u8 {
        return &self.rx_buf;
    }
    pub fn takeStaged(self: *Model) ?pump.Staged {
        defer self.staged = null;
        return self.staged;
    }
    pub fn transfer(self: *Model) bool {
        if (self.fail_transfer) return false;
        if (self.sent < self.sent_len.len) {
            self.sent_len[self.sent] = std.mem.readInt(u16, self.tx_buf[frame.Hdr.len..][0..2], .little);
        }
        self.sent += 1;
        self.rx_buf = self.reply;
        return true;
    }
    pub fn dispatch(self: *Model, view: frame.View) bool {
        _ = view;
        self.dispatched += 1;
        return self.stop_on_dispatch;
    }
};

fn dataReply(model: *Model, len: u16) void {
    @memset(model.reply[Frame.header_bytes..][0..len], 0x5C);
    _ = frame.seal(&model.reply, 1, 0, len);
}

test "an absent co-processor gives up after three missed handshakes" {
    var model: Model = .{ .armed = false };
    var stats: pump.Stats = .{};
    try std.testing.expectEqual(pump.Outcome.timeout, pump.run(&model, 64, &stats));
    try std.testing.expectEqual(@as(u16, 3), stats.hs_timeouts);
    try std.testing.expectEqual(@as(u16, 0), stats.transfers);
    try std.testing.expectEqual(@as(u32, 3 * pump.Timing.hs_wait_ms), model.slept_ms);
}

test "idle replies run the whole budget with filler frames" {
    var model: Model = .{};
    frame.filler(&model.reply);
    var stats: pump.Stats = .{};
    try std.testing.expectEqual(pump.Outcome.ok, pump.run(&model, 5, &stats));
    try std.testing.expectEqual(@as(u16, 5), stats.transfers);
    try std.testing.expectEqual(@as(u16, 5), stats.idle);
    try std.testing.expectEqual(@as(u16, 0), model.sent_len[0]);
    try std.testing.expectEqual(@as(u32, 5 * pump.Timing.gap_ms), model.slept_ms);
}

test "a staged payload is sealed once, then filler follows" {
    var model: Model = .{ .staged = .{ .if_type = 2, .len = 40 } };
    frame.filler(&model.reply);
    var stats: pump.Stats = .{};
    _ = pump.run(&model, 2, &stats);
    try std.testing.expectEqual(@as(u16, 40), model.sent_len[0]);
    try std.testing.expectEqual(@as(u16, 0), model.sent_len[1]);
    try std.testing.expect(model.staged == null);
}

test "a transport fault stops the run before counting it" {
    var model: Model = .{ .fail_transfer = true };
    var stats: pump.Stats = .{};
    try std.testing.expectEqual(pump.Outcome.bus_fault, pump.run(&model, 4, &stats));
    try std.testing.expectEqual(@as(u16, 0), stats.transfers);
}

test "a data frame that answers the wait stops the pump" {
    var model: Model = .{};
    dataReply(&model, 16);
    var stats: pump.Stats = .{};
    try std.testing.expectEqual(pump.Outcome.ok, pump.run(&model, 10, &stats));
    try std.testing.expectEqual(@as(u16, 1), stats.transfers);
    try std.testing.expectEqual(@as(u16, 1), stats.data);
    try std.testing.expectEqual(@as(usize, 1), model.dispatched);
    try std.testing.expectEqual(@as(u32, 0), model.slept_ms);
}

test "frames that fail integrity are counted and never routed" {
    var model: Model = .{ .stop_on_dispatch = false };
    dataReply(&model, 16);
    model.reply[Frame.header_bytes] ^= 0xFF;
    var stats: pump.Stats = .{};
    _ = pump.run(&model, 2, &stats);
    try std.testing.expectEqual(@as(u16, 2), stats.bad_checksum);

    model = .{ .stop_on_dispatch = false };
    dataReply(&model, 16);
    model.reply[frame.Hdr.offset] = 5;
    stats = .{};
    _ = pump.run(&model, 2, &stats);
    try std.testing.expectEqual(@as(u16, 2), stats.malformed);
    try std.testing.expectEqual(@as(usize, 0), model.dispatched);
}

test "a handshake that comes back resets the give-up count" {
    const pattern = [_]bool{false} ** 400 ++ [_]bool{true} ++ [_]bool{false} ** 400 ++ [_]bool{true};
    var model: Model = .{ .miss_pattern = &pattern, .armed = false };
    frame.filler(&model.reply);
    var stats: pump.Stats = .{};
    try std.testing.expectEqual(pump.Outcome.ok, pump.run(&model, 6, &stats));
    try std.testing.expectEqual(@as(u16, 2), stats.transfers);
    try std.testing.expectEqual(@as(u16, 4), stats.hs_timeouts);
}
