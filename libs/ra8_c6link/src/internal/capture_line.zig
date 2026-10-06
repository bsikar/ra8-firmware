//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Text records for the capture transport: one `c6cap` line per frame or
//! HANDSHAKE edge, written to any `sink` with `put([]const u8)` in pieces so
//! no caller holds a whole 3200-character line.

const std = @import("std");

/// Frame bytes per hex piece handed to the sink.
pub const chunk: usize = 64;

const digits = "0123456789abcdef";

/// `bytes` with its trailing zero bytes dropped.
pub fn used(bytes: []const u8) []const u8 {
    var end = bytes.len;
    while (end > 0 and bytes[end - 1] == 0) end -= 1;
    return bytes[0..end];
}

/// Write `c6cap <seq> <kind>[ <hex>]\n` for one frame.
pub fn frame(sink: anytype, seq: u32, kind: []const u8, bytes: []const u8) void {
    var head: [32]u8 = undefined;
    sink.put(std.fmt.bufPrint(&head, "c6cap {d} {s}", .{ seq, kind }) catch unreachable);
    const data = used(bytes);
    if (data.len != 0) sink.put(" ");
    var hex: [chunk * 2]u8 = undefined;
    var at: usize = 0;
    while (at < data.len) {
        const piece = data[at..@min(at + chunk, data.len)];
        for (piece, 0..) |byte, i| {
            hex[i * 2] = digits[byte >> 4];
            hex[i * 2 + 1] = digits[byte & 0xF];
        }
        sink.put(hex[0 .. piece.len * 2]);
        at += piece.len;
    }
    sink.put("\n");
}

/// Write `c6cap <seq> hs <0|1>\n` for a HANDSHAKE level change.
pub fn edge(sink: anytype, seq: u32, level: bool) void {
    var line: [32]u8 = undefined;
    sink.put(std.fmt.bufPrint(&line, "c6cap {d} hs {d}\n", .{ seq, @intFromBool(level) }) catch unreachable);
}
