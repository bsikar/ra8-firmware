//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dma.zig.

const std = @import("std");
const dma = @import("dma");

const Ops = struct {
    mstp_err: u16 = 0,
    start_err: u16 = 0,
    stop_err: u16 = 0,
    enabled: usize = 0,
    disabled: usize = 0,
    started: usize = 0,
    stopped: usize = 0,
    last_cfg: dma.Config = .{},
    errors: usize = 0,
    fail_err: u16 = 0,

    pub fn mstpEnable(self: *Ops, id: u16) u16 {
        std.debug.assert(id == dma.mstp_dmac0_dtc0);
        self.enabled += 1;
        return self.mstp_err;
    }
    pub fn mstpDisable(self: *Ops, id: u16) u16 {
        std.debug.assert(id == dma.mstp_dmac0_dtc0);
        self.disabled += 1;
        return self.mstp_err;
    }
    pub fn dmacStart(self: *Ops, channel: u8, cfg: *const dma.Config) u16 {
        std.debug.assert(channel < dma.channel_count);
        self.started += 1;
        self.last_cfg = cfg.*;
        return self.start_err;
    }
    pub fn dmacStop(self: *Ops, _: u8) u16 {
        self.stopped += 1;
        return self.stop_err;
    }
    pub fn nullPtr(self: *Ops, _: [*:0]const u8) u16 {
        self.errors += 1;
        return dma.null_ptr;
    }
    pub fn logError(self: *Ops, _: [*:0]const u8) void {
        self.errors += 1;
    }
    pub fn logInfo(_: *Ops, _: [*:0]const u8) void {}
    pub fn fail(self: *Ops, _: [*:0]const u8, err: u16) void {
        self.errors += 1;
        self.fail_err = err;
    }
};

var hits: u32 = 0;
fn onDone(ctx: ?*anyopaque) callconv(.c) void {
    const p: *u32 = @ptrCast(@alignCast(ctx.?));
    p.* += 1;
}

const word_req = dma.Request{ .src_addr = 0x2000_0000, .dst_addr = 0x4000_1000, .count = 16, .width = dma.width_word, .src_inc = true, .on_complete = &onDone, .ctx = &hits };

fn ready(ops: *Ops) !dma.State {
    var s: dma.State = .{};
    try std.testing.expectEqual(dma.ok, s.init(ops));
    return s;
}

test "a request before init is refused and nulls are logged" {
    var ops: Ops = .{};
    var s: dma.State = .{};
    var ch: u8 = 0;
    try std.testing.expectEqual(dma.not_initialized, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(dma.null_ptr, s.request(&ops, null, &ch));
    try std.testing.expectEqual(dma.null_ptr, s.request(&ops, &word_req, null));
    try std.testing.expectEqual(@as(usize, 2), ops.errors);
    try std.testing.expectEqual(@as(usize, 0), ops.started);
}

test "init failing at MSTP reports hw_init_failed" {
    var ops: Ops = .{ .mstp_err = 0x201 };
    var s: dma.State = .{};
    try std.testing.expectEqual(dma.hw_init_failed, s.init(&ops));
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqual(@as(u16, 0x201), ops.fail_err);
}

test "requests take channels in order and pack the DMAC config" {
    var ops: Ops = .{};
    var s = try ready(&ops);
    var ch: u8 = 0xAA;
    try std.testing.expectEqual(dma.ok, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(@as(u8, 0), ch);
    try std.testing.expectEqual(@as(u32, 0x2000_0000), ops.last_cfg.src);
    try std.testing.expectEqual(@as(u32, 0x4000_1000), ops.last_cfg.dst);
    try std.testing.expectEqual(@as(u16, 16), ops.last_cfg.count);
    try std.testing.expectEqual(dma.width_word, ops.last_cfg.width);
    try std.testing.expect(ops.last_cfg.src_inc and !ops.last_cfg.dst_inc);
    try std.testing.expectEqual(@as(u8, 0), ops.last_cfg.mode);
    try std.testing.expectEqual(@as(u16, 16), s.peek(0).?.count);
    try std.testing.expectEqual(dma.ok, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(@as(u8, 1), ch);
}

test "zero count and a too-wide element are invalid" {
    var ops: Ops = .{};
    var s = try ready(&ops);
    var ch: u8 = 0;
    var r = word_req;
    r.count = 0;
    try std.testing.expectEqual(dma.invalid_arg, s.request(&ops, &r, &ch));
    r = word_req;
    r.width = 3;
    try std.testing.expectEqual(dma.invalid_arg, s.request(&ops, &r, &ch));
    try std.testing.expectEqual(@as(usize, 0), ops.started);
}

test "eight channels, then no_mem; a failed start keeps the channel free" {
    var ops: Ops = .{};
    var s = try ready(&ops);
    var ch: u8 = 0;
    ops.start_err = 0x204;
    try std.testing.expectEqual(dma.hw_error, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(@as(?*const dma.Request, null), s.peek(0));
    ops.start_err = 0;
    for (0..dma.channel_count) |_| try std.testing.expectEqual(dma.ok, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(dma.no_mem, s.request(&ops, &word_req, &ch));
}

test "release frees a channel, refuses a free or bad one, and maps stop errors" {
    var ops: Ops = .{};
    var s = try ready(&ops);
    var ch: u8 = 0;
    try std.testing.expectEqual(dma.ok, s.request(&ops, &word_req, &ch));
    try std.testing.expectEqual(dma.invalid_arg, s.release(&ops, dma.channel_count));
    try std.testing.expectEqual(dma.invalid_state, s.release(&ops, 1));
    ops.stop_err = 0x203;
    try std.testing.expectEqual(dma.hw_error, s.release(&ops, ch));
    var busy = false;
    try std.testing.expectEqual(dma.ok, s.isBusy(&ops, ch, &busy));
    try std.testing.expect(busy);
    ops.stop_err = 0;
    try std.testing.expectEqual(dma.ok, s.release(&ops, ch));
    try std.testing.expectEqual(dma.ok, s.isBusy(&ops, ch, &busy));
    try std.testing.expect(!busy);
    try std.testing.expectEqual(dma.invalid_arg, s.isBusy(&ops, dma.channel_count, &busy));
    try std.testing.expectEqual(dma.null_ptr, s.isBusy(&ops, 0, null));
}

test "dispatch runs the stored callback; deinit stops live channels" {
    var ops: Ops = .{};
    var s = try ready(&ops);
    var ch: u8 = 0;
    hits = 0;
    try std.testing.expectEqual(dma.ok, s.request(&ops, &word_req, &ch));
    s.dispatch(ch);
    s.dispatch(5);
    s.dispatch(dma.channel_count);
    try std.testing.expectEqual(@as(u32, 1), hits);
    ops.stop_err = 0x204;
    try std.testing.expectEqual(dma.ok, s.deinit(&ops));
    try std.testing.expectEqual(@as(usize, 1), ops.stopped);
    try std.testing.expect(!s.initialized);
    try std.testing.expectEqual(@as(?*const dma.Request, null), s.peek(ch));
    s.initialized = true;
    ops.mstp_err = 0x204;
    try std.testing.expectEqual(dma.hw_error, s.deinit(&ops));
    try std.testing.expect(s.initialized);
}
