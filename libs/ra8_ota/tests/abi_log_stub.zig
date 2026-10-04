//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Stand-in for `ra8_log_emit_error` in the ABI tests: counts the calls and
//! keeps the last tag and message (truncated like the C buffers were, 63 and
//! 95 bytes) for the tests to read back. Ported from abi_log_stub.c
//! (RA8FW-628).

const std = @import("std");

var log_calls: usize = 0;
var last_tag: [64]u8 = .{0} ** 64;
var last_message: [96]u8 = .{0} ** 96;

fn keep(buffer: []u8, text: [*:0]const u8) void {
    const span = std.mem.span(text);
    const len = @min(span.len, buffer.len - 1);
    @memcpy(buffer[0..len], span[0..len]);
    buffer[len] = 0;
}

fn emitError(tag_text: [*:0]const u8, message_text: [*:0]const u8) callconv(.c) void {
    log_calls += 1;
    keep(&last_tag, tag_text);
    keep(&last_message, message_text);
}

comptime {
    @export(&emitError, .{ .name = "ra8_log_emit_error" });
}

pub fn reset() void {
    log_calls = 0;
    last_tag[0] = 0;
    last_message[0] = 0;
}

pub fn calls() usize {
    return log_calls;
}

pub fn tag() [:0]const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(&last_tag)));
}

pub fn message() [:0]const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(&last_message)));
}
