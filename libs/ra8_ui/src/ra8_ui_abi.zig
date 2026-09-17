//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_ui/inc/ra8_ui.h`. The logic lives in
//! `internal/root.zig`; this file owns the exported symbols, the pointer guards
//! in their original order, the `ra8_err_t` mapping, and the diagnostic lines
//! the C emitted through `RA8_CHECK_NULL_PTR`.
//!
//! Guard order is part of the contract: `ra8_ui_hit_test` rejects `out_action`
//! before `out_hit` and both before `targets`, so the host suite can tell the
//! three rejections apart by which argument it nulled.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Rectangle in framebuffer pixels (`ra8_ui_rect_t`).
pub const Rect = implementation.Rect;
/// Tap target (`ra8_ui_target_t`).
pub const Target = implementation.Target;
/// Screen-id stack (`ra8_ui_nav_t`).
pub const Nav = implementation.Nav;
/// Page cursor (`ra8_ui_pager_t`).
pub const Pager = implementation.Pager;

/// Subset of `ra8_err_t` this library returns.
pub const UiError = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_ui";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn rejectNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(UiError.null_ptr);
}

/// Test whether a point lies inside a rectangle; a null rectangle is a miss.
pub export fn ra8_ui_rect_contains(r: ?*const Rect, px: i32, py: i32) callconv(.c) bool {
    const rect = r orelse return false;
    return rect.contains(px, py);
}

/// Find the first tap target containing a point.
pub export fn ra8_ui_hit_test(
    targets: ?[*]const Target,
    count: u16,
    px: i32,
    py: i32,
    out_action: ?*u16,
    out_hit: ?*bool,
) callconv(.c) u16 {
    const action_out = out_action orelse return rejectNull("out_action must not be nullptr");
    const hit_out = out_hit orelse return rejectNull("out_hit must not be nullptr");

    var list: []const Target = &.{};
    if (count > 0) {
        const base = targets orelse
            return rejectNull("targets must not be nullptr when count > 0");
        list = base[0..count];
    }

    hit_out.* = false;
    if (implementation.hitTest(list, px, py)) |action| {
        action_out.* = action;
        hit_out.* = true;
    }
    return @intFromEnum(UiError.ok);
}

/// Initialise a navigation stack with a root screen.
pub export fn ra8_ui_nav_init(nav: ?*Nav, root_screen: u16) callconv(.c) u16 {
    const stack = nav orelse return rejectNull("nav must not be nullptr");
    implementation.navInit(stack, root_screen);
    return @intFromEnum(UiError.ok);
}

/// Push a new screen onto the stack.
pub export fn ra8_ui_nav_push(nav: ?*Nav, screen: u16) callconv(.c) u16 {
    const stack = nav orelse return rejectNull("nav must not be nullptr");
    implementation.navPush(stack, screen) catch |fault| return switch (fault) {
        error.Full => @intFromEnum(UiError.no_mem),
        else => @intFromEnum(UiError.invalid_state),
    };
    return @intFromEnum(UiError.ok);
}

/// Pop the top screen, reporting the one revealed beneath.
pub export fn ra8_ui_nav_pop(nav: ?*Nav, out_screen: ?*u16) callconv(.c) u16 {
    const stack = nav orelse return rejectNull("nav must not be nullptr");
    const out = out_screen orelse return rejectNull("out_screen must not be nullptr");
    out.* = implementation.navPop(stack) catch return @intFromEnum(UiError.invalid_state);
    return @intFromEnum(UiError.ok);
}

/// Replace the top screen in place.
pub export fn ra8_ui_nav_replace(nav: ?*Nav, screen: u16) callconv(.c) u16 {
    const stack = nav orelse return rejectNull("nav must not be nullptr");
    implementation.navReplace(stack, screen) catch return @intFromEnum(UiError.invalid_state);
    return @intFromEnum(UiError.ok);
}

/// Read the current (top) screen id.
pub export fn ra8_ui_nav_top(nav: ?*const Nav, out_screen: ?*u16) callconv(.c) u16 {
    const stack = nav orelse return rejectNull("nav must not be nullptr");
    const out = out_screen orelse return rejectNull("out_screen must not be nullptr");
    out.* = implementation.navTop(stack) catch return @intFromEnum(UiError.invalid_state);
    return @intFromEnum(UiError.ok);
}

/// Initialise a pager over `total` pages at page 0.
pub export fn ra8_ui_pager_init(p: ?*Pager, total: u16) callconv(.c) u16 {
    const pager = p orelse return rejectNull("p must not be nullptr");
    implementation.pagerInit(pager, total) catch return @intFromEnum(UiError.invalid_arg);
    return @intFromEnum(UiError.ok);
}

/// Advance to the next page, clamping at the last.
pub export fn ra8_ui_pager_next(p: ?*Pager, out_changed: ?*bool) callconv(.c) u16 {
    const pager = p orelse return rejectNull("p must not be nullptr");
    const changed = out_changed orelse return rejectNull("out_changed must not be nullptr");
    changed.* = implementation.pagerNext(pager);
    return @intFromEnum(UiError.ok);
}

/// Step to the previous page, clamping at page 0.
pub export fn ra8_ui_pager_prev(p: ?*Pager, out_changed: ?*bool) callconv(.c) u16 {
    const pager = p orelse return rejectNull("p must not be nullptr");
    const changed = out_changed orelse return rejectNull("out_changed must not be nullptr");
    changed.* = implementation.pagerPrev(pager);
    return @intFromEnum(UiError.ok);
}

/// Jump to an absolute page, clamping into `[0, total-1]`.
pub export fn ra8_ui_pager_goto(p: ?*Pager, page: u16, out_changed: ?*bool) callconv(.c) u16 {
    const pager = p orelse return rejectNull("p must not be nullptr");
    const changed = out_changed orelse return rejectNull("out_changed must not be nullptr");
    changed.* = implementation.pagerGoto(pager, page);
    return @intFromEnum(UiError.ok);
}

comptime {
    // `ra8_err_t` is 16-bit across the repo; these are the only codes the
    // library can return.
    std.debug.assert(@intFromEnum(UiError.ok) == 0);
    std.debug.assert(@intFromEnum(UiError.no_mem) == 0x102);
    std.debug.assert(@intFromEnum(UiError.invalid_arg) == 0x103);
    std.debug.assert(@intFromEnum(UiError.invalid_state) == 0x104);
    std.debug.assert(@intFromEnum(UiError.null_ptr) == 0x504);
    std.debug.assert(implementation.nav_max_depth == 8);
}
