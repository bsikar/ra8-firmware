//! Viewport state machine for the tap-to-zoom image viewer (RA8FW-1253).
//!
//! Port of the pure geometry core of zoom.c: the magnified-plane axis window,
//! the dimension bound, the saturating anchor add and the anchor clamp. Pure
//! integer arithmetic with no MMIO and no allocation, so it behaves identically
//! on the unit-test host, in ra8_emulator and on silicon. The strip composite
//! stays in zoom_render.c (C this slice) and the zoom_view_* C ABI seams land
//! with the view state machine in the next slice.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");

/// Viewport fraction retained across a pan step (k_zoom_overlap_div).
pub const overlap_div: i32 = 8;
/// Ladder multiplier: doubling, not increment (k_zoom_scale_dbl).
pub const scale_dbl: i32 = 2;
/// Ladder floor (k_zoom_scale_min).
pub const scale_min: i32 = 1;
/// Engine ceiling on integer magnification (k_zoom_scale_max).
pub const scale_max: i32 = 8;
/// Largest source dimension the engine addresses (k_zoom_dim_max).
pub const dim_max: u32 = 65535;
/// Destination rows composited per strip (k_zoom_strip_rows_max).
pub const strip_rows_max: u32 = 32;
/// Letterbox fill (k_zoom_bg_gray): white, an exact 16-level palette step.
pub const bg_gray: u8 = 255;
/// Settle window that promotes a fast flush to quality GC16 (ms).
pub const settle_ms: u32 = 350;

/// One axis of the magnified plane: the destination window (d0..d1) in
/// magnified-plane pixels and the source span (s0, count) it samples.
pub const Axis = struct {
    d0: i32 = 0,
    d1: i32 = 0,
    s0: u32 = 0,
    count: u32 = 0,
};

/// Clip the magnified plane to the viewport window and compute the source
/// span it samples (priv_zoom_axis). `anchor` is the magnified-plane
/// coordinate at viewport offset 0, `extent` the viewport size in destination
/// pixels, `src_dim * scale` the magnified-plane extent on this axis.
pub fn axis(anchor: i32, extent: i32, scale: i32, src_dim: i32) Axis {
    const plane = src_dim * scale;
    var d0: i32 = -anchor;
    if (d0 < 0) {
        d0 = 0;
    }
    var d1: i32 = plane - anchor;
    if (d1 > extent) {
        d1 = extent;
    }
    if (d1 < d0) {
        d1 = d0;
    }
    var out = Axis{ .d0 = d0, .d1 = d1, .s0 = 0 };
    // Beyond this point anchor + d0 >= 0 by construction, so the source-index
    // divisions never touch a negative numerator.
    if (d1 <= d0) {
        out.count = 0;
        return out;
    }
    const s0 = @divTrunc(anchor + d0, scale);
    const s1 = @divTrunc(anchor + d1 - 1, scale);
    // Positive by construction: s1 >= s0 whenever d1 > d0.
    out.s0 = @intCast(s0);
    out.count = @intCast(s1 - s0 + 1);
    return out;
}

/// Whether a source dimension is inside the engine's addressable range
/// (internal_dim_ok). Zero has no pixels to magnify; past the cap the
/// magnified plane could overflow at maximum zoom.
pub fn dimOk(dim: u32) bool {
    return dim != 0 and dim <= dim_max;
}

/// Saturating signed addition (internal_sat_add). A pan delta is
/// caller-supplied and may be arbitrary, so the anchor addition is done in
/// 64 bits and clamped rather than allowed to wrap.
pub fn satAdd(a: i32, b: i32) i32 {
    const sum: i64 = @as(i64, a) + @as(i64, b);
    if (sum > std.math.maxInt(i32)) {
        return std.math.maxInt(i32);
    }
    if (sum < std.math.minInt(i32)) {
        return std.math.minInt(i32);
    }
    return @intCast(sum);
}

/// Clamp one anchor against the magnified plane and the viewport extent
/// (internal_clamp_anchor). When the magnified image is bigger than the
/// viewport the anchor is held inside [0, plane - extent]; when it is
/// smaller the image is centred and the anchor goes negative, which the
/// render reads as a letterbox.
pub fn clampAnchor(anchor: i32, plane: i32, extent: i32) i32 {
    if (plane <= extent) {
        return -@divTrunc(extent - plane, 2);
    }
    if (anchor < 0) {
        return 0;
    }
    if (anchor > plane - extent) {
        return plane - extent;
    }
    return anchor;
}

test "axis clips the plane to the viewport and names the source span" {
    // 4x magnification of a 16 px source is a 64 px plane; a 32 px viewport
    // anchored at 0 sees plane pixels 0..31 and source pixels 0..8.
    const a = axis(0, 32, 4, 16);
    try std.testing.expectEqual(@as(i32, 0), a.d0);
    try std.testing.expectEqual(@as(i32, 32), a.d1);
    try std.testing.expectEqual(@as(u32, 0), a.s0);
    try std.testing.expectEqual(@as(u32, 8), a.count);
}

test "axis past the right edge clamps to a zero-count window" {
    // Anchor at the plane edge: d0 clamps to 0? No -- anchor 64 on a 64 px
    // plane gives d0 = -64 -> 0 and d1 = 0, so d1 <= d0 and the count is 0.
    const a = axis(64, 32, 4, 16);
    try std.testing.expectEqual(@as(u32, 0), a.count);
}

test "axis on a mid-page anchor keeps the overlap window" {
    // Anchor 32, viewport 32 on the 64 px plane: d0 = 0, d1 = 32, so the
    // window is plane 32..63 and the source span is 8..16.
    const a = axis(32, 32, 4, 16);
    try std.testing.expectEqual(@as(i32, 0), a.d0);
    try std.testing.expectEqual(@as(i32, 32), a.d1);
    try std.testing.expectEqual(@as(u32, 8), a.s0);
    try std.testing.expectEqual(@as(u32, 8), a.count);
}

test "dimOk bounds" {
    try std.testing.expect(!dimOk(0));
    try std.testing.expect(dimOk(1));
    try std.testing.expect(dimOk(dim_max));
    try std.testing.expect(!dimOk(dim_max + 1));
}

test "satAdd clamps at the int32 boundary instead of wrapping" {
    try std.testing.expectEqual(@as(i32, 3), satAdd(1, 2));
    try std.testing.expectEqual(std.math.maxInt(i32), satAdd(std.math.maxInt(i32), 1));
    try std.testing.expectEqual(std.math.minInt(i32), satAdd(std.math.minInt(i32), -1));
}

test "clampAnchor centres a small image and bounds a big one" {
    // Smaller than the viewport: centred, anchor negative (letterbox).
    try std.testing.expectEqual(@as(i32, -4), clampAnchor(0, 24, 32));
    // Bigger: held inside [0, plane - extent].
    try std.testing.expectEqual(@as(i32, 0), clampAnchor(-9, 64, 32));
    try std.testing.expectEqual(@as(i32, 32), clampAnchor(99, 64, 32));
    try std.testing.expectEqual(@as(i32, 10), clampAnchor(10, 64, 32));
}
