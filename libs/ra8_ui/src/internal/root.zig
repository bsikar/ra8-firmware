//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure interaction core for `ra8_ui`: rectangle hit-testing, a fixed-depth
//! screen-id stack, and a clamped page cursor. Nothing here touches the C ABI,
//! logging, or hardware, so every branch is reachable from a Zig test.
//!
//! The three data types are `extern struct` because the caller owns them and
//! passes them straight through the C header; their layout is the ABI and the
//! comptime block at the bottom asserts it.

const std = @import("std");

/// Max screen-stack depth (`k_ra8_ui_nav_max_depth`).
pub const nav_max_depth: u8 = 8;

/// Axis-aligned rectangle, top-left inclusive and bottom-right exclusive.
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,

    /// Containment test: `(px >= x) && (px < x+w) && (py >= y) && (py < y+h)`.
    ///
    /// The edges are computed in 64-bit. The C added two `int32_t` values, so a
    /// rectangle whose right edge passed `INT32_MAX` was undefined there; widening
    /// keeps an enormous width meaning "everything to the right" instead.
    pub fn contains(self: *const Rect, px: i32, py: i32) bool {
        const left: i64 = self.x;
        const top: i64 = self.y;
        const right: i64 = left + @as(i64, self.w);
        const bottom: i64 = top + @as(i64, self.h);
        const point_x: i64 = px;
        const point_y: i64 = py;
        return (point_x >= left) and (point_x < right) and (point_y >= top) and (point_y < bottom);
    }
};

/// One tap target: a rectangle bound to an opaque action id.
pub const Target = extern struct {
    rect: Rect,
    action_id: u16,
    reserved: u16,
};

/// First target containing the point, or null when nothing is hit.
///
/// Earlier entries win on overlap, which is the documented ordering contract.
pub fn hitTest(targets: []const Target, px: i32, py: i32) ?u16 {
    for (targets) |*target| {
        if (target.rect.contains(px, py)) {
            return target.action_id;
        }
    }
    return null;
}

/// Why a navigation call refused. The ABI maps these onto `ra8_err_t`.
pub const NavFault = error{
    /// `depth == 0`: the stack was never initialised.
    NotInitialised,
    /// Already at `nav_max_depth`.
    Full,
    /// A pop at the root, which is never removed.
    AtRoot,
};

/// Fixed-depth screen-id stack, caller-owned.
pub const Nav = extern struct {
    stack: [nav_max_depth]u16,
    depth: u8,
    reserved: [3]u8,
};

/// Seat `root_screen` at the stack base and set the depth to one.
///
/// Only slot 0 is written; the rest keep whatever the caller's storage held,
/// exactly as the C did.
pub fn navInit(nav: *Nav, root_screen: u16) void {
    nav.stack[0] = root_screen;
    nav.depth = 1;
}

/// Push a screen, refusing an uninitialised or full stack.
pub fn navPush(nav: *Nav, screen: u16) NavFault!void {
    if (nav.depth == 0) {
        return NavFault.NotInitialised;
    }
    if (nav.depth >= nav_max_depth) {
        return NavFault.Full;
    }
    nav.stack[nav.depth] = screen;
    nav.depth += 1;
}

/// Pop the top screen and answer the one revealed beneath it.
pub fn navPop(nav: *Nav) NavFault!u16 {
    if (nav.depth <= 1) {
        return NavFault.AtRoot;
    }
    nav.depth -= 1;
    return nav.stack[nav.depth - 1];
}

/// Swap the top screen in place, leaving the depth alone.
pub fn navReplace(nav: *Nav, screen: u16) NavFault!void {
    if (nav.depth == 0) {
        return NavFault.NotInitialised;
    }
    nav.stack[nav.depth - 1] = screen;
}

/// Read the current top screen.
pub fn navTop(nav: *const Nav) NavFault!u16 {
    if (nav.depth == 0) {
        return NavFault.NotInitialised;
    }
    return nav.stack[nav.depth - 1];
}

/// Why a pager call refused.
pub const PagerFault = error{
    /// `total == 0`, which leaves no page to sit on.
    EmptyTotal,
};

/// Clamped `(current, total)` page cursor.
pub const Pager = extern struct {
    current: u16,
    total: u16,
};

/// Initialise a pager at page zero over `total` pages.
pub fn pagerInit(pager: *Pager, total: u16) PagerFault!void {
    if (total == 0) {
        return PagerFault.EmptyTotal;
    }
    pager.current = 0;
    pager.total = total;
}

/// Advance one page, clamping at the last. Answers whether the cursor moved.
///
/// `total > 0` is checked first so `total - 1` never wraps, which is the
/// decision the host suite's MC/DC vectors pin down.
pub fn pagerNext(pager: *Pager) bool {
    if ((pager.total > 0) and (pager.current < (pager.total - 1))) {
        pager.current += 1;
        return true;
    }
    return false;
}

/// Step back one page, clamping at zero. Answers whether the cursor moved.
pub fn pagerPrev(pager: *Pager) bool {
    if (pager.current > 0) {
        pager.current -= 1;
        return true;
    }
    return false;
}

/// Jump to an absolute page, clamping into `[0, total-1]` when `total > 0`.
///
/// A zero total keeps the requested page verbatim: the C did the same, and the
/// goto MC/DC vectors assert exactly that arm.
pub fn pagerGoto(pager: *Pager, page: u16) bool {
    var target = page;
    if ((pager.total > 0) and (target > (pager.total - 1))) {
        target = pager.total - 1;
    }
    const changed = target != pager.current;
    pager.current = target;
    return changed;
}

comptime {
    // These layouts are the published C ABI: callers allocate the structs.
    std.debug.assert(@sizeOf(Rect) == 16);
    std.debug.assert(@offsetOf(Rect, "x") == 0);
    std.debug.assert(@offsetOf(Rect, "y") == 4);
    std.debug.assert(@offsetOf(Rect, "w") == 8);
    std.debug.assert(@offsetOf(Rect, "h") == 12);

    std.debug.assert(@sizeOf(Target) == 20);
    std.debug.assert(@offsetOf(Target, "rect") == 0);
    std.debug.assert(@offsetOf(Target, "action_id") == 16);
    std.debug.assert(@offsetOf(Target, "reserved") == 18);

    std.debug.assert(@sizeOf(Nav) == 20);
    std.debug.assert(@offsetOf(Nav, "stack") == 0);
    std.debug.assert(@offsetOf(Nav, "depth") == 16);
    std.debug.assert(@offsetOf(Nav, "reserved") == 17);

    std.debug.assert(@sizeOf(Pager) == 4);
    std.debug.assert(@offsetOf(Pager, "current") == 0);
    std.debug.assert(@offsetOf(Pager, "total") == 2);
}
