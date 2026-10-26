//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_uart` facade, driven through a fake binding that keeps
//! a transmit echo buffer and a receive queue. Everything proved here is
//! facade behaviour no chip can change: the entry guards, the malformed
//! binding and config rejections, the zero-length no-ops, and the membrane's
//! refusal of a count that exceeds the buffer it was handed.

const std = @import("std");
const abi = @import("abi");

const Uart = abi.Uart;
const Iface = abi.Iface;
const Config = abi.Config;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_found: u16 = 0x106;
const err_not_initialized: u16 = 0x10F;
const err_invalid_state: u16 = 0x104;
const err_busy: u16 = 0x109;

const line_115200 = Config{
    .baud = 115_200,
    .data_bits = abi.DataBits.eight,
    .parity = abi.Parity.none,
    .stop_bits = abi.StopBits.one,
};

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    last_uart: u8 = 0,
    last_cfg: Config = .{ .baud = 0, .data_bits = 0, .parity = 0, .stop_bits = 0 },
    sent: [16]u8 = @splat(0),
    sent_len: usize = 0,
    rx: [4]u8 = .{ 'r', 'x', 0, 0 },
    rx_len: usize = 2,
    configure_err: u16 = ok,
    write_err: u16 = ok,
    read_err: u16 = ok,
    lie_count: bool = false,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeConfigure(ctx: ?*anyopaque, uart: u8, cfg: Config) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_uart = uart;
    state.last_cfg = cfg;
    return state.configure_err;
}

fn fakeWrite(ctx: ?*anyopaque, uart: u8, bytes: ?[*]const u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_uart = uart;
    if (state.write_err != ok) return state.write_err;
    if (bytes) |buf| {
        for (0..len) |i| {
            if (i < state.sent.len) state.sent[i] = buf[i];
        }
        state.sent_len = @min(len, state.sent.len);
    }
    return ok;
}

fn fakeRead(ctx: ?*anyopaque, uart: u8, out: ?[*]u8, len: usize, out_count: ?*usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_uart = uart;
    if (state.read_err != ok) return state.read_err;
    const n = @min(len, state.rx_len);
    if (out) |buf| {
        for (0..n) |i| buf[i] = state.rx[i];
    }
    out_count.?.* = if (state.lie_count) len + 1 else n;
    return ok;
}

const fake_ops = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = fakeRead };

fn bound() Uart {
    fake = .{};
    var uart = std.mem.zeroes(Uart);
    std.debug.assert(abi.fw_uart_bind(&uart, &fake_ops, &fake) == ok);
    return uart;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var uart = std.mem.zeroes(Uart);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_bind(&uart, null, null));
    try std.testing.expect(!uart.bound);

    const no_configure = Iface{ .configure = null, .write = fakeWrite, .read = fakeRead };
    const no_write = Iface{ .configure = fakeConfigure, .write = null, .read = fakeRead };
    const no_read = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_bind(&uart, &no_configure, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_bind(&uart, &no_write, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_bind(&uart, &no_read, null));
    try std.testing.expect(!uart.bound);
}

test "an unbound handle refuses configure, write and read" {
    const uart = std.mem.zeroes(Uart);
    try std.testing.expectEqual(err_not_initialized, abi.fw_uart_configure(&uart, 0, line_115200));
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_uart_write(&uart, 0, @as([*]const u8, @ptrCast(&byte)), 1),
    );
    var buffer: [8]u8 = @splat(0);
    var count: usize = 9;
    try std.testing.expectEqual(err_not_initialized, abi.fw_uart_read(&uart, 0, &buffer, 8, &count));
    try std.testing.expectEqual(@as(usize, 0), count);

    // A zero-capacity poll is the same no-op a zero-length write is: ok with
    // a zero count, unbound handle or not, because it wakes nothing.
    count = 9;
    try std.testing.expectEqual(ok, abi.fw_uart_read(&uart, 0, null, 0, &count));
    try std.testing.expectEqual(@as(usize, 0), count);
}

test "configure refuses a zero baud, a zeroed cfg and an out-of-range field" {
    var uart = bound();
    var bad = line_115200;
    bad.baud = 0;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_configure(&uart, 0, bad));

    const zeroed = Config{ .baud = 0, .data_bits = 0, .parity = 0, .stop_bits = 0 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_configure(&uart, 0, zeroed));

    var bad_width = line_115200;
    bad_width.data_bits = 9;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_configure(&uart, 0, bad_width));

    var bad_parity = line_115200;
    bad_parity.parity = 9;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_configure(&uart, 0, bad_parity));

    var bad_stop = line_115200;
    bad_stop.stop_bits = 9;
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_configure(&uart, 0, bad_stop));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "configure carries the instance and the line through to the binding" {
    var uart = bound();
    try std.testing.expectEqual(ok, abi.fw_uart_configure(&uart, 1, line_115200));
    try std.testing.expectEqual(@as(u8, 1), fake.last_uart);
    try std.testing.expectEqual(@as(u32, 115_200), fake.last_cfg.baud);
    try std.testing.expectEqual(@as(u8, abi.DataBits.eight), fake.last_cfg.data_bits);
    try std.testing.expectEqual(@as(u8, abi.Parity.none), fake.last_cfg.parity);
    try std.testing.expectEqual(@as(u8, abi.StopBits.one), fake.last_cfg.stop_bits);

    fake.configure_err = err_not_found;
    try std.testing.expectEqual(err_not_found, abi.fw_uart_configure(&uart, 9, line_115200));
}

test "a zero-byte write is a no-op that does not wake the binding" {
    var uart = bound();
    try std.testing.expectEqual(ok, abi.fw_uart_write(&uart, 0, null, 0));
    try std.testing.expectEqual(ok, abi.fw_uart_write(&uart, 0, undefined, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a null buffer with a nonzero length is refused before the binding" {
    var uart = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_write(&uart, 0, null, 8));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "write carries the bytes through to the binding" {
    var uart = bound();
    const payload = "hello";
    try std.testing.expectEqual(ok, abi.fw_uart_write(&uart, 0, payload, payload.len));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 0), fake.last_uart);
    try std.testing.expectEqual(@as(usize, 5), fake.sent_len);
    try std.testing.expectEqualStrings("hello", fake.sent[0..5]);

    fake.write_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_uart_write(&uart, 0, payload, payload.len));
}

test "read with no out_count is refused before the binding" {
    var uart = bound();
    var buffer: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_uart_read(&uart, 0, &buffer, 8, null));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "read reports what arrived, including a quiet line" {
    var uart = bound();
    var buffer: [8]u8 = @splat(0);
    var count: usize = 9;
    try std.testing.expectEqual(ok, abi.fw_uart_read(&uart, 0, &buffer, buffer.len, &count));
    try std.testing.expectEqual(@as(usize, 2), count);
    try std.testing.expectEqualStrings("rx", buffer[0..2]);

    fake.rx_len = 0;
    count = 9;
    try std.testing.expectEqual(ok, abi.fw_uart_read(&uart, 0, &buffer, buffer.len, &count));
    try std.testing.expectEqual(@as(usize, 0), count);

    fake.read_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_uart_read(&uart, 0, &buffer, buffer.len, &count));
}

test "a count above the buffer's capacity is a lying binding, refused" {
    var uart = bound();
    var buffer: [8]u8 = @splat(0);
    var count: usize = 0;
    fake.lie_count = true;
    try std.testing.expectEqual(err_invalid_state, abi.fw_uart_read(&uart, 0, &buffer, buffer.len, &count));
    try std.testing.expectEqual(@as(usize, 0), count);
}
