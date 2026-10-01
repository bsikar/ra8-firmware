//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The console stream handle. The behaviour worth pinning is the refusal:
//! the sink writes straight at an SCI channel, so binding before the console
//! is up has to fail rather than hand back a handle onto a dead channel.

const std = @import("std");
const console_stream = @import("console_stream");

var console_up: bool = true;
var bound_channel: u8 = 0;
var bind_calls: usize = 0;
var bound_state: ?*anyopaque = null;

export fn priv_ra8_board_uart_console_is_up() bool {
    return console_up;
}

export fn ra8_io_stream_uart_init(s: *anyopaque, state: *anyopaque, channel: u8) u32 {
    _ = s;
    bound_state = state;
    bound_channel = channel;
    bind_calls += 1;
    return 0;
}

test "a null out is refused" {
    console_up = true;
    try std.testing.expectEqual(console_stream.Err.null_ptr, console_stream.bind(null));
}

test "binding before the console is up is refused" {
    console_up = false;
    bind_calls = 0;
    var handle: console_stream.IoStream = .{};
    try std.testing.expectEqual(console_stream.Err.not_initialized, console_stream.bind(&handle));
    try std.testing.expectEqual(@as(usize, 0), bind_calls);
}

test "the handle binds to the VCOM channel" {
    console_up = true;
    var handle: console_stream.IoStream = .{};
    try std.testing.expectEqual(console_stream.Err.ok, console_stream.bind(&handle));
    try std.testing.expectEqual(@as(u8, 8), bound_channel);
    try std.testing.expectEqual(console_stream.sci_channel, bound_channel);
}

test "an app without ra8_io gets a refusal, not a link error" {
    console_up = true;
    bind_calls = 0;
    var handle: console_stream.IoStream = .{};
    try std.testing.expectEqual(
        console_stream.Err.not_supported,
        console_stream.bindThrough(null, &handle),
    );
    try std.testing.expectEqual(@as(usize, 0), bind_calls);
}

test "a null out is refused before the sink is even looked at" {
    try std.testing.expectEqual(
        console_stream.Err.null_ptr,
        console_stream.bindThrough(null, null),
    );
}

test "two handles share one module-owned sink" {
    console_up = true;
    var first: console_stream.IoStream = .{};
    var second: console_stream.IoStream = .{};
    _ = console_stream.bind(&first);
    const first_state = bound_state;
    _ = console_stream.bind(&second);
    try std.testing.expectEqual(first_state, bound_state);
}
