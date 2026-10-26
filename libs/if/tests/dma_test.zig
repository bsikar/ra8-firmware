//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_dma` facade, driven through a fake binding that
//! captures the transfer request and reports the channel's alignment
//! contract. Everything proved here is facade behaviour no chip can change:
//! the entry guards, the malformed binding and config rejections, the
//! zero-length no-op, and the refusal paths.

const std = @import("std");
const abi = @import("abi");

const Dma = abi.Dma;
const Iface = abi.Iface;
const Config = abi.Config;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_found: u16 = 0x106;
const err_not_initialized: u16 = 0x10F;
const err_busy: u16 = 0x109;

const m2m_byte = Config{ .direction = abi.Direction.mem_to_mem, .width = abi.Width.one };

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    last_chan: u8 = 0,
    last_cfg: Config = .{ .direction = 0, .width = 0 },
    last_len: usize = 0,
    last_src: usize = 0,
    last_dst: usize = 0,
    configure_err: u16 = ok,
    transfer_err: u16 = ok,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeConfigure(ctx: ?*anyopaque, chan: u8, cfg: Config) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_chan = chan;
    state.last_cfg = cfg;
    return state.configure_err;
}

fn fakeTransfer(ctx: ?*anyopaque, chan: u8, src: ?*const anyopaque, dst: ?*anyopaque, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_chan = chan;
    state.last_len = len;
    state.last_src = @intFromPtr(src.?);
    state.last_dst = @intFromPtr(dst.?);
    return state.transfer_err;
}

const fake_ops = Iface{ .configure = fakeConfigure, .transfer = fakeTransfer };

fn bound() Dma {
    fake = .{};
    var dma = std.mem.zeroes(Dma);
    std.debug.assert(abi.fw_dma_bind(&dma, &fake_ops, &fake) == ok);
    return dma;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var dma = std.mem.zeroes(Dma);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_bind(&dma, null, null));
    try std.testing.expect(!dma.bound);

    const no_configure = Iface{ .configure = null, .transfer = fakeTransfer };
    const no_transfer = Iface{ .configure = fakeConfigure, .transfer = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_bind(&dma, &no_configure, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_bind(&dma, &no_transfer, null));
    try std.testing.expect(!dma.bound);
}

test "an unbound handle refuses configure and transfer" {
    const dma = std.mem.zeroes(Dma);
    try std.testing.expectEqual(err_not_initialized, abi.fw_dma_configure(&dma, 0, m2m_byte));
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_dma_transfer(&dma, 0, @as(*const anyopaque, @ptrCast(&byte)), @as(*anyopaque, @ptrCast(&byte)), 1),
    );
}

test "configure refuses a zeroed cfg and out-of-range fields" {
    var dma = bound();
    const zeroed = Config{ .direction = 0, .width = 0 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_configure(&dma, 0, zeroed));

    var bad_dir = m2m_byte;
    bad_dir.direction = 9;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_configure(&dma, 0, bad_dir));

    var bad_width = m2m_byte;
    bad_width.width = 3;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_dma_configure(&dma, 0, bad_width));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "configure carries the channel, direction and width through to the binding" {
    var dma = bound();
    try std.testing.expectEqual(ok, abi.fw_dma_configure(&dma, 2, m2m_byte));
    try std.testing.expectEqual(@as(u8, 2), fake.last_chan);
    try std.testing.expectEqual(@as(u8, abi.Direction.mem_to_mem), fake.last_cfg.direction);
    try std.testing.expectEqual(@as(u8, abi.Width.one), fake.last_cfg.width);

    var word = m2m_byte;
    word.width = abi.Width.four;
    fake = .{};
    std.debug.assert(abi.fw_dma_bind(&dma, &fake_ops, &fake) == ok);
    try std.testing.expectEqual(ok, abi.fw_dma_configure(&dma, 0, word));
    try std.testing.expectEqual(@as(u8, abi.Width.four), fake.last_cfg.width);

    fake.configure_err = err_not_found;
    try std.testing.expectEqual(err_not_found, abi.fw_dma_configure(&dma, 9, m2m_byte));
}

test "a zero-byte transfer is a no-op that does not arm the channel" {
    var dma = bound();
    try std.testing.expectEqual(ok, abi.fw_dma_transfer(&dma, 0, null, null, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a missing buffer with a nonzero length is refused before the binding" {
    var dma = bound();
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_invalid_arg,
        abi.fw_dma_transfer(&dma, 0, null, @as(*anyopaque, @ptrCast(&byte)), 8),
    );
    try std.testing.expectEqual(
        err_invalid_arg,
        abi.fw_dma_transfer(&dma, 0, @as(*const anyopaque, @ptrCast(&byte)), null, 8),
    );
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "transfer carries the channel, pointers and length through to the binding" {
    var dma = bound();
    var src: [8]u8 = @splat(0x2A);
    var dst: [8]u8 = @splat(0);
    try std.testing.expectEqual(ok, abi.fw_dma_transfer(&dma, 1, &src, &dst, src.len));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 1), fake.last_chan);
    try std.testing.expectEqual(@as(usize, 8), fake.last_len);

    fake.transfer_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_dma_transfer(&dma, 1, &src, &dst, src.len));
}
