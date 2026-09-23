//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_keyboard/inc/ra8_keyboard.h`. The layout and
//! typing logic lives in `internal/root.zig`; this file owns the exported
//! symbols, the pointer guards in their original order, the `ra8_err_t`
//! mapping, and the diagnostic lines the C emitted through
//! `RA8_CHECK_NULL_PTR`.
//!
//! Two contract details are preserved deliberately. `ra8_kbd_layout_init`
//! rejects `kb` before `frame`, and both before the zero-area frame, so the
//! host suite can tell a null argument from a bad rectangle. And the hit test
//! still calls `ra8_ui_rect_contains` as an external symbol rather than
//! reimplementing it, so the rectangle contract stays owned by `ra8_ui` and the
//! host suites keep substituting the real implementation at link time.

const std = @import("std");
const implementation = @import("internal/root.zig");

comptime {
    std.debug.assert(@sizeOf(Rect) == 16);
    std.debug.assert(@sizeOf(Key) == 20);
    std.debug.assert(@sizeOf(Layout) == 820);
    std.debug.assert(@offsetOf(Layout, "frame") == 804);
    std.debug.assert(@sizeOf(Text) == 66);
}

/// Rectangle in framebuffer pixels (`ra8_ui_rect_t`).
pub const Rect = implementation.Rect;
/// One key descriptor (`ra8_kbd_key_t`).
pub const Key = implementation.Key;
/// Key grid for the active layer (`ra8_kbd_layout_t`).
pub const Layout = implementation.Layout;
/// Caller-owned text state (`ra8_kbd_text_t`).
pub const Text = implementation.Text;
/// Active layer (`ra8_kbd_layer_t`).
pub const Layer = implementation.Layer;
/// Key behaviour (`ra8_kbd_key_kind_t`).
pub const KeyKind = implementation.KeyKind;
/// `ra8_kbd_hit` "no key" sentinel.
pub const no_hit = implementation.no_hit;

/// Subset of `ra8_err_t` this library returns.
pub const KeyboardError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "KBD";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Rectangle hit test owned by `ra8_ui`; the real one is linked in.
extern fn ra8_ui_rect_contains(r: ?*const Rect, px: i32, py: i32) bool;

fn containsSeam(r: ?*const Rect, px: i32, py: i32) callconv(.c) bool {
    return ra8_ui_rect_contains(r, px, py);
}

fn rejectNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(KeyboardError.null_ptr);
}

/// Lay out the letters layer inside `frame`.
pub export fn ra8_kbd_layout_init(kb: ?*Layout, frame: ?*const Rect) callconv(.c) u16 {
    const layout = kb orelse return rejectNull("kb must not be nullptr");
    const rect = frame orelse return rejectNull("frame must not be nullptr");
    implementation.layoutInit(layout, rect) catch |fault| return switch (fault) {
        error.NoArea => @intFromEnum(KeyboardError.invalid_arg),
    };
    return @intFromEnum(KeyboardError.ok);
}

/// Index of the key under a point, or the `no_hit` sentinel.
pub export fn ra8_kbd_hit(kb: ?*const Layout, px: i32, py: i32) callconv(.c) u8 {
    const layout = kb orelse return implementation.no_hit;
    return implementation.hit(layout, px, py, containsSeam);
}

/// Shift-correct glyph for a key, or 0 for a special key or a bad index.
pub export fn ra8_kbd_key_glyph(kb: ?*const Layout, key_idx: u8) callconv(.c) u8 {
    const layout = kb orelse return 0;
    return implementation.glyphOf(layout, key_idx);
}

/// Reset a text buffer to empty and uncommitted.
pub export fn ra8_kbd_text_init(t: ?*Text) callconv(.c) u16 {
    const text = t orelse return rejectNull("t must not be nullptr");
    implementation.textInit(text);
    return @intFromEnum(KeyboardError.ok);
}

/// Apply a key press to the text and layout state.
pub export fn ra8_kbd_apply(t: ?*Text, kb: ?*Layout, key_idx: u8) callconv(.c) u16 {
    const text = t orelse return rejectNull("t must not be nullptr");
    const layout = kb orelse return rejectNull("kb must not be nullptr");
    implementation.applyKey(text, layout, key_idx);
    return @intFromEnum(KeyboardError.ok);
}
