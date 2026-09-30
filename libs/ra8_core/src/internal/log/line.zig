//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Log line framing: `[tag] LEVEL: message` with an optional `=value`.
//!
//! The bytes stream through a caller-supplied sink rather than a buffer,
//! because tag and message are caller strings of any length and buffering
//! them would truncate lines the C emitted whole.

const format = @import("log_format");

/// Where framed bytes go, one at a time.
pub const Sink = *const fn (byte: u8) void;

const frame = struct {
    pub const tag_open: u8 = '[';
    pub const tag_close: u8 = ']';
    pub const space: u8 = ' ';
    pub const colon: u8 = ':';
    pub const equals: u8 = '=';
    pub const carriage_return: u8 = '\r';
    pub const line_feed: u8 = '\n';
};

fn putAll(sink: Sink, bytes: []const u8) void {
    for (bytes) |byte| sink(byte);
}

/// `[tag] LEVEL: message` and the line ending.
pub fn plain(sink: Sink, level: []const u8, tag: []const u8, message: []const u8) void {
    prefix(sink, level, tag, message);
    terminate(sink);
}

/// `[tag] LEVEL: message=value` with an unsigned value.
pub fn withUnsigned(
    sink: Sink,
    level: []const u8,
    tag: []const u8,
    message: []const u8,
    value: u32,
) void {
    prefix(sink, level, tag, message);
    sink(frame.equals);
    var digits: [format.limits.u32_digits]u8 = undefined;
    putAll(sink, format.unsigned(&digits, value));
    terminate(sink);
}

/// `[tag] LEVEL: message=value` with a signed value.
pub fn withSigned(
    sink: Sink,
    level: []const u8,
    tag: []const u8,
    message: []const u8,
    value: i32,
) void {
    prefix(sink, level, tag, message);
    sink(frame.equals);
    var chars: [format.limits.i32_chars]u8 = undefined;
    putAll(sink, format.signed(&chars, value));
    terminate(sink);
}

fn prefix(sink: Sink, level: []const u8, tag: []const u8, message: []const u8) void {
    sink(frame.tag_open);
    putAll(sink, tag);
    sink(frame.tag_close);
    sink(frame.space);
    putAll(sink, level);
    sink(frame.colon);
    sink(frame.space);
    putAll(sink, message);
}

fn terminate(sink: Sink) void {
    sink(frame.carriage_return);
    sink(frame.line_feed);
}
