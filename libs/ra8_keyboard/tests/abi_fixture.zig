//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Supplies the two symbols the keyboard ABI leaves undefined in host tests,
//! `ra8_log_emit_error` and `ra8_ui_rect_contains`, and records what they saw
//! for the guard-order and MC/DC assertions. Ported from abi_fixture.c
//! (RA8FW-633).

const Rect = @import("abi").Rect;

pub var log_calls: usize = 0;
pub var last_message: [*:0]const u8 = "";
pub var contains_calls: usize = 0;

pub fn reset() void {
    log_calls = 0;
    last_message = "";
    contains_calls = 0;
}

fn emitError(tag_text: [*:0]const u8, message_text: [*:0]const u8) callconv(.c) void {
    _ = tag_text;
    log_calls += 1;
    last_message = message_text;
}

fn rectContains(r: ?*const Rect, px: i32, py: i32) callconv(.c) bool {
    contains_calls += 1;
    const rect = r orelse return false;
    const left: i64 = rect.x;
    const top: i64 = rect.y;
    const right = left + rect.w;
    const bottom = top + rect.h;
    return px >= left and px < right and py >= top and py < bottom;
}

comptime {
    @export(&emitError, .{ .name = "ra8_log_emit_error" });
    @export(&rectContains, .{ .name = "ra8_ui_rect_contains" });
}
