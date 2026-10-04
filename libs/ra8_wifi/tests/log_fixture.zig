//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Stand-in for `ra8_log_emit_error` in the ABI tests: keeps up to 64 bytes
//! of the last message so rejected-pointer diagnostics can be checked without
//! the production log sink. Test-only. Ported from log_fixture.c (RA8FW-635).

var last_log: [64]u8 = undefined;
var last_log_len: usize = 0;

fn emitError(tag_text: ?[*:0]const u8, message_text: ?[*:0]const u8) callconv(.c) void {
    _ = tag_text;
    last_log_len = 0;
    const message = message_text orelse return;
    while (last_log_len < last_log.len and message[last_log_len] != 0) {
        last_log[last_log_len] = message[last_log_len];
        last_log_len += 1;
    }
}

comptime {
    @export(&emitError, .{ .name = "ra8_log_emit_error" });
}

/// Clear the captured diagnostic; the backing bytes are ignored afterward.
pub fn reset() void {
    last_log_len = 0;
}

/// The captured diagnostic bytes (not NUL-terminated, at most 64).
pub fn last() []const u8 {
    return last_log[0..last_log_len];
}
