//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the stub UART profile and the ops that serve it.

const std = @import("std");
const stub = @import("uart_stub");
const abi = @import("uart_stub_abi");

const Err = abi.Err;

/// Stand-in for the facade's `fw_uart_bind`, which lives in `libs/if` and is
/// linked by the composition root, not by this unit's host tests. It records
/// what the bind convenience handed it so the bind path is testable without
/// the real facade.
var bind_calls: usize = 0;
var last_iface: ?*const abi.Iface = null;

pub export fn fw_uart_bind(uart: ?*abi.Handle, iface: ?*const abi.Iface, ctx: ?*anyopaque) callconv(.c) u16 {
    bind_calls += 1;
    last_iface = iface;

    if (uart) |h| {
        h.iface = iface;
        h.ctx = ctx;
        h.bound = true;
    }

    return Err.ok;
}

fn cfg(baud: u32, data_bits: u8, parity: u8, stop_bits: u8) abi.Config {
    return .{ .baud = baud, .data_bits = data_bits, .parity = parity, .stop_bits = stop_bits };
}

test "profile carries lines 0 and 1" {
    stub.reset();

    try stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_7, .parity = stub.parity_even, .stop_bits = stub.stop_bits_two });
    try stub.configure(1, .{ .baud = 115200, .data_bits = stub.data_bits_8, .parity = stub.parity_none, .stop_bits = stub.stop_bits_one });
}

test "profile refuses lines the stub board does not carry" {
    stub.reset();

    try std.testing.expectError(error.NotFound, stub.configure(2, .{ .baud = 9600, .data_bits = stub.data_bits_8, .parity = stub.parity_none, .stop_bits = stub.stop_bits_one }));
    try std.testing.expectError(error.NotFound, stub.write(9, "hi"));
    var out: [4]u8 = undefined;
    try std.testing.expectError(error.NotFound, stub.read(200, &out));
    try std.testing.expectError(error.NotFound, stub.feed(3, "x"));
}

test "a line comes up at the stub default of 115200 8N1" {
    stub.reset();

    const line = stub.line(0).?;
    try std.testing.expectEqual(@as(u32, 115200), line.baud);
    try std.testing.expectEqual(stub.data_bits_8, line.data_bits);
    try std.testing.expectEqual(stub.parity_none, line.parity);
    try std.testing.expectEqual(stub.stop_bits_one, line.stop_bits);
}

test "configure rejects a zero baud and out-of-range fields" {
    stub.reset();

    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 0, .data_bits = stub.data_bits_8, .parity = stub.parity_none, .stop_bits = stub.stop_bits_one }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_none, .parity = stub.parity_none, .stop_bits = stub.stop_bits_one }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 9600, .data_bits = 3, .parity = stub.parity_none, .stop_bits = stub.stop_bits_one }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_8, .parity = 3, .stop_bits = stub.stop_bits_one }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_8, .parity = stub.parity_none, .stop_bits = stub.stop_bits_none }));
    try std.testing.expectError(error.InvalidArg, stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_8, .parity = stub.parity_none, .stop_bits = 3 }));
}

test "configure puts a line on its description" {
    stub.reset();

    try stub.configure(1, .{ .baud = 19200, .data_bits = stub.data_bits_7, .parity = stub.parity_odd, .stop_bits = stub.stop_bits_two });
    const line = stub.line(1).?;
    try std.testing.expectEqual(@as(u32, 19200), line.baud);
    try std.testing.expectEqual(stub.data_bits_7, line.data_bits);
    try std.testing.expectEqual(stub.parity_odd, line.parity);
    try std.testing.expectEqual(stub.stop_bits_two, line.stop_bits);
}

test "write captures bytes in order" {
    stub.reset();

    try stub.write(0, "console");
    try std.testing.expectEqualStrings("console", stub.line(0).?.tx[0..7]);
    try std.testing.expectEqual(@as(usize, 7), stub.transmitted(0).?);
}

test "a zero-length write costs nothing" {
    stub.reset();

    try stub.write(0, "");
    try std.testing.expectEqual(@as(usize, 0), stub.transmitted(0).?);
}

test "the tx ring wraps at capacity" {
    stub.reset();

    const pattern = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    try stub.write(0, pattern);
    try stub.write(0, "XY");
    try std.testing.expectEqual(@as(usize, 66), stub.transmitted(0).?);
    try std.testing.expectEqual(@as(u8, 'X'), stub.line(0).?.tx[0]);
    try std.testing.expectEqual(@as(u8, 'Y'), stub.line(0).?.tx[1]);
}

test "a quiet line reads zero, not an error" {
    stub.reset();

    var out: [8]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 0), try stub.read(0, &out));
}

test "read drains a primed receive queue in order" {
    stub.reset();

    try stub.feed(1, "modem");
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 5), try stub.read(1, &out));
    try std.testing.expectEqualStrings("modem", out[0..5]);
    try std.testing.expectEqual(@as(usize, 0), try stub.read(1, &out));
}

test "read caps the count at the buffer's length" {
    stub.reset();

    try stub.feed(0, "abcdef");
    var out: [3]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 3), try stub.read(0, &out));
    try std.testing.expectEqualStrings("abc", out[0..3]);
    try std.testing.expectEqual(@as(usize, 3), try stub.read(0, &out));
    try std.testing.expectEqualStrings("def", out[0..3]);
}

test "a feed past capacity drops the overflow" {
    stub.reset();

    try stub.feed(0, "0123456789abcdef");
    try stub.feed(0, "XY");
    var out: [16]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 16), try stub.read(0, &out));
    try std.testing.expectEqualStrings("0123456789abcdef", out[0..16]);
}

test "configure does not disturb a line's queues" {
    stub.reset();

    try stub.feed(0, "abc");
    try stub.write(0, "xy");
    try stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_7, .parity = stub.parity_even, .stop_bits = stub.stop_bits_two });
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 3), try stub.read(0, &out));
    try std.testing.expectEqual(@as(usize, 2), stub.transmitted(0).?);
}

test "reset restores the default board" {
    stub.reset();
    try stub.configure(0, .{ .baud = 9600, .data_bits = stub.data_bits_7, .parity = stub.parity_even, .stop_bits = stub.stop_bits_two });
    try stub.write(0, "hello");
    try stub.feed(0, "rx");
    stub.reset();

    try std.testing.expectEqual(@as(u32, 115200), stub.line(0).?.baud);
    try std.testing.expectEqual(@as(usize, 0), stub.transmitted(0).?);
    var out: [4]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 0), try stub.read(0, &out));
}

test "op configure hands a config through the facade shape" {
    stub.reset();

    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_configure(null, 0, cfg(9600, stub.data_bits_8, stub.parity_none, stub.stop_bits_one)));
    try std.testing.expectEqual(@as(u32, 9600), stub.line(0).?.baud);
}

test "op configure answers not_found for a line the board does not carry" {
    stub.reset();

    try std.testing.expectEqual(Err.not_found, abi.fw_uart_stub_configure(null, 2, cfg(9600, stub.data_bits_8, stub.parity_none, stub.stop_bits_one)));
}

test "op write captures bytes and refuses a null buffer with a length" {
    stub.reset();

    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_write(null, 0, "abc", 3));
    try std.testing.expectEqual(@as(usize, 3), stub.transmitted(0).?);
    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_write(null, 0, null, 0));
    try std.testing.expectEqual(@as(usize, 3), stub.transmitted(0).?);
    try std.testing.expectEqual(Err.invalid_arg, abi.fw_uart_stub_write(null, 0, null, 4));
}

test "op read drains and reports the count" {
    stub.reset();

    try stub.feed(1, "rx");
    var out: [4]u8 = undefined;
    var count: usize = 999;
    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_read(null, 1, &out, 4, &count));
    try std.testing.expectEqual(@as(usize, 2), count);
    try std.testing.expectEqualStrings("rx", out[0..2]);
}

test "op read refuses a null count and answers zero on a quiet line" {
    stub.reset();

    var out: [4]u8 = undefined;
    var count: usize = 0;
    try std.testing.expectEqual(Err.invalid_arg, abi.fw_uart_stub_read(null, 0, &out, 4, null));
    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_read(null, 0, &out, 4, &count));
    try std.testing.expectEqual(@as(usize, 0), count);
    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_read(null, 0, null, 0, &count));
    try std.testing.expectEqual(Err.invalid_arg, abi.fw_uart_stub_read(null, 0, null, 4, &count));
}

test "op read answers not_found for a line the board does not carry" {
    stub.reset();

    var out: [4]u8 = undefined;
    var count: usize = 0;
    try std.testing.expectEqual(Err.not_found, abi.fw_uart_stub_read(null, 7, &out, 4, &count));
}

test "the stub iface is one singleton the facade can borrow" {
    const first = abi.fw_uart_stub_iface();
    const second = abi.fw_uart_stub_iface();
    try std.testing.expect(first == second);

    // The ops are non-optional function pointers by type, so "no op unset"
    // holds structurally; the malformed-binding guard is the C facade's job.
    try std.testing.expectEqual(Err.not_found, first.configure(null, 5, cfg(9600, stub.data_bits_8, stub.parity_none, stub.stop_bits_one)));
}

test "the bind convenience hands the facade the stub ops" {
    stub.reset();
    bind_calls = 0;
    last_iface = null;

    var h: abi.Handle = .{ .iface = null, .ctx = null, .bound = false };
    try std.testing.expectEqual(Err.ok, abi.fw_uart_stub_bind(&h));
    try std.testing.expectEqual(@as(usize, 1), bind_calls);
    try std.testing.expect(h.bound);
    try std.testing.expect(last_iface == abi.fw_uart_stub_iface());
    try std.testing.expect(h.iface == last_iface);
    try std.testing.expect(h.ctx == null);
}
