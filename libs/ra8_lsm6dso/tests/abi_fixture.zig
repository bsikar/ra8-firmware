//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Stand-in for `ra8_log_emit_error` in the ABI tests: counts the calls and
//! keeps the last message pointer for the tests to read back. Ported from
//! abi_fixture.c (RA8FW-631).

var log_count: u32 = 0;
var log_last: [*:0]const u8 = "";

fn emitError(tag_text: [*:0]const u8, message_text: [*:0]const u8) callconv(.c) void {
    _ = tag_text;
    log_count += 1;
    log_last = message_text;
}

comptime {
    @export(&emitError, .{ .name = "ra8_log_emit_error" });
}

pub fn reset() void {
    log_count = 0;
    log_last = "";
}

pub fn count() u32 {
    return log_count;
}

pub fn last() [*:0]const u8 {
    return log_last;
}
