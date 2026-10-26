//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_spi` facade, driven through a fake binding that keeps
//! a transmit echo buffer and shifts a fixed pattern into the receive side.
//! Everything proved here is facade behaviour no chip can change: the entry
//! guards, the malformed binding and config rejections, the zero-length
//! no-ops, and the exchange's both-sides-required rule.

const std = @import("std");
const abi = @import("abi");

const Spi = abi.Spi;
const Iface = abi.Iface;
const Config = abi.Config;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_found: u16 = 0x106;
const err_not_initialized: u16 = 0x10F;
const err_busy: u16 = 0x109;

const mode_8mhz = Config{ .clock_hz = 8_000_000, .mode = abi.Mode.mode_0 };

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    last_dev: u8 = 0,
    last_cfg: Config = .{ .clock_hz = 0, .mode = 0 },
    sent: [16]u8 = @splat(0),
    sent_len: usize = 0,
    shifted: [16]u8 = @splat(0),
    configure_err: u16 = ok,
    write_err: u16 = ok,
    exchange_err: u16 = ok,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeConfigure(ctx: ?*anyopaque, dev: u8, cfg: Config) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_dev = dev;
    state.last_cfg = cfg;
    return state.configure_err;
}

fn fakeWrite(ctx: ?*anyopaque, dev: u8, bytes: ?[*]const u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_dev = dev;
    if (state.write_err != ok) return state.write_err;
    if (bytes) |buf| {
        for (0..len) |i| {
            if (i < state.sent.len) state.sent[i] = buf[i];
        }
        state.sent_len = @min(len, state.sent.len);
    }
    return ok;
}

fn fakeExchange(ctx: ?*anyopaque, dev: u8, tx: ?[*]const u8, rx: ?[*]u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_dev = dev;
    if (state.exchange_err != ok) return state.exchange_err;
    if (tx) |src| {
        for (0..len) |i| {
            if (i < state.shifted.len) state.shifted[i] = src[i];
        }
        state.sent_len = @min(len, state.sent.len);
    }
    if (rx) |dst| {
        // A device under a full-duplex read answers 0xA5 per byte.
        for (0..len) |i| dst[i] = 0xA5;
    }
    return ok;
}

const fake_ops = Iface{ .configure = fakeConfigure, .write = fakeWrite, .exchange = fakeExchange };

fn bound() Spi {
    fake = .{};
    var dev = std.mem.zeroes(Spi);
    std.debug.assert(abi.fw_spi_bind(&dev, &fake_ops, &fake) == ok);
    return dev;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var dev = std.mem.zeroes(Spi);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_bind(&dev, null, null));
    try std.testing.expect(!dev.bound);

    const no_configure = Iface{ .configure = null, .write = fakeWrite, .exchange = fakeExchange };
    const no_write = Iface{ .configure = fakeConfigure, .write = null, .exchange = fakeExchange };
    const no_exchange = Iface{ .configure = fakeConfigure, .write = fakeWrite, .exchange = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_bind(&dev, &no_configure, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_bind(&dev, &no_write, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_bind(&dev, &no_exchange, null));
    try std.testing.expect(!dev.bound);
}

test "an unbound handle refuses configure, write and exchange" {
    const dev = std.mem.zeroes(Spi);
    try std.testing.expectEqual(err_not_initialized, abi.fw_spi_configure(&dev, 0, mode_8mhz));
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_spi_write(&dev, 0, @as([*]const u8, @ptrCast(&byte)), 1),
    );
    var rx: [8]u8 = @splat(0);
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_spi_exchange(&dev, 0, @as([*]const u8, @ptrCast(&byte)), &rx, 1),
    );
}

test "configure refuses a zero clock, a zeroed cfg and an out-of-range mode" {
    var dev = bound();
    var bad = mode_8mhz;
    bad.clock_hz = 0;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_configure(&dev, 0, bad));

    const zeroed = Config{ .clock_hz = 0, .mode = 0 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_configure(&dev, 0, zeroed));

    var bad_mode = mode_8mhz;
    bad_mode.mode = 9;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_configure(&dev, 0, bad_mode));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "configure carries the device and the line through to the binding" {
    var dev = bound();
    try std.testing.expectEqual(ok, abi.fw_spi_configure(&dev, 2, mode_8mhz));
    try std.testing.expectEqual(@as(u8, 2), fake.last_dev);
    try std.testing.expectEqual(@as(u32, 8_000_000), fake.last_cfg.clock_hz);
    try std.testing.expectEqual(@as(u8, abi.Mode.mode_0), fake.last_cfg.mode);

    fake.configure_err = err_not_found;
    try std.testing.expectEqual(err_not_found, abi.fw_spi_configure(&dev, 9, mode_8mhz));
}

test "a zero-byte write or exchange is a no-op that does not wake the binding" {
    var dev = bound();
    try std.testing.expectEqual(ok, abi.fw_spi_write(&dev, 0, null, 0));
    try std.testing.expectEqual(ok, abi.fw_spi_write(&dev, 0, undefined, 0));
    try std.testing.expectEqual(ok, abi.fw_spi_exchange(&dev, 0, null, null, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a null buffer with a nonzero length is refused before the binding" {
    var dev = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_write(&dev, 0, null, 8));

    var rx: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_exchange(&dev, 0, null, &rx, 8));
    const tx = [_]u8{ 0x9F, 0x00 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_spi_exchange(&dev, 0, &tx, null, 8));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "write carries the bytes through to the binding" {
    var dev = bound();
    const cmd = [_]u8{ 0x9F, 0x00, 0x01 };
    try std.testing.expectEqual(ok, abi.fw_spi_write(&dev, 0, &cmd, cmd.len));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 0), fake.last_dev);
    try std.testing.expectEqual(@as(usize, 3), fake.sent_len);
    try std.testing.expectEqualSlices(u8, &cmd, fake.sent[0..3]);

    fake.write_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_spi_write(&dev, 0, &cmd, cmd.len));
}

test "exchange shifts both directions in one call" {
    var dev = bound();
    const cmd = [_]u8{ 0x9F, 0x00, 0x00 };
    var rx: [3]u8 = @splat(0);
    try std.testing.expectEqual(ok, abi.fw_spi_exchange(&dev, 1, &cmd, &rx, 3));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 1), fake.last_dev);
    try std.testing.expectEqualSlices(u8, &cmd, fake.shifted[0..3]);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xA5, 0xA5, 0xA5 }, &rx);

    fake.exchange_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_spi_exchange(&dev, 1, &cmd, &rx, 3));
}
