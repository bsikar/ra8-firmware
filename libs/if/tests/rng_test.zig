//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_rng` facade, driven through a fake binding that fills
//! from a pattern byte. Everything proved here is facade behaviour no chip can
//! change: the entry guards, the malformed-binding rejection, the zero-byte
//! no-op and the membrane's own hands-off rule for the buffer on error.

const std = @import("std");
const abi = @import("abi");

const Rng = abi.Rng;
const Iface = abi.Iface;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_initialized: u16 = 0x10F;
const err_busy: u16 = 0x109;

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    fill_err: u16 = ok,
    ready_err: u16 = ok,
    ready_answer: bool = true,
    pattern: u8 = 0xA5,
    last_len: usize = 0,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeFill(ctx: ?*anyopaque, out: ?[*]u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_len = len;
    if (state.fill_err != ok) return state.fill_err;
    if (out) |buf| {
        var i: usize = 0;
        while (i < len) : (i += 1) buf[i] = state.pattern;
    }
    return ok;
}

fn fakeReady(ctx: ?*anyopaque, out_ready: ?*bool) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    if (state.ready_err != ok) return state.ready_err;
    out_ready.?.* = state.ready_answer;
    return ok;
}

const fake_ops = Iface{ .fill = fakeFill, .ready = fakeReady };

fn bound() Rng {
    fake = .{};
    var rng = std.mem.zeroes(Rng);
    std.debug.assert(abi.fw_rng_bind(&rng, &fake_ops, &fake) == ok);
    return rng;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var rng = std.mem.zeroes(Rng);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_bind(&rng, null, null));
    try std.testing.expect(!rng.bound);

    const no_fill = Iface{ .fill = null, .ready = fakeReady };
    const no_ready = Iface{ .fill = fakeFill, .ready = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_bind(&rng, &no_fill, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_bind(&rng, &no_ready, null));
    try std.testing.expect(!rng.bound);
}

test "an unbound handle refuses bytes and ready" {
    const rng = std.mem.zeroes(Rng);
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_rng_bytes(&rng, @as([*]u8, @ptrCast(&byte)), 1),
    );
    var ready = true;
    try std.testing.expectEqual(err_not_initialized, abi.fw_rng_ready(&rng, &ready));
    try std.testing.expect(!ready);
}

test "a zero-byte fill is a no-op that does not wake the binding" {
    var rng = bound();
    try std.testing.expectEqual(ok, abi.fw_rng_bytes(&rng, null, 0));
    try std.testing.expectEqual(ok, abi.fw_rng_bytes(&rng, undefined, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a null buffer with a nonzero length is refused before the binding" {
    var rng = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_bytes(&rng, null, 16));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "bytes carries the request through and the binding fills every byte" {
    var rng = bound();
    var buffer: [16]u8 = @splat(0);
    try std.testing.expectEqual(ok, abi.fw_rng_bytes(&rng, &buffer, buffer.len));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(usize, 16), fake.last_len);
    for (buffer) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
}

test "a binding failure comes back as its own error and the membrane never writes" {
    var rng = bound();
    fake.fill_err = err_busy;
    var buffer: [8]u8 = @splat(0x5A);
    try std.testing.expectEqual(err_busy, abi.fw_rng_bytes(&rng, &buffer, buffer.len));
    for (buffer) |byte| try std.testing.expectEqual(@as(u8, 0x5A), byte);
}

test "ready reports the binding's answer and clears out_ready first" {
    var rng = bound();
    var ready = true;
    try std.testing.expectEqual(ok, abi.fw_rng_ready(&rng, &ready));
    try std.testing.expect(ready);

    fake.ready_answer = false;
    try std.testing.expectEqual(ok, abi.fw_rng_ready(&rng, &ready));
    try std.testing.expect(!ready);

    fake.ready_err = err_busy;
    ready = true;
    try std.testing.expectEqual(err_busy, abi.fw_rng_ready(&rng, &ready));
    try std.testing.expect(!ready);

    try std.testing.expectEqual(err_invalid_arg, abi.fw_rng_ready(&rng, null));
}
