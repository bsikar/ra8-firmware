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

/// One axis-aligned rectangle in framebuffer pixel coordinates
/// (ra8_ui_rect_t): inclusive on the top-left corner, exclusive on the
/// bottom-right, so w = h = 0 contains no points.
pub const Rect = struct {
    x: i32 = 0,
    y: i32 = 0,
    w: i32 = 0,
    h: i32 = 0,
};

/// The gray8 sub-rectangle reader behind a bound source (zoom_read_fn).
/// The typed error seam lands with the view ABI slice; the raw pointer
/// keeps the struct layout honest in the meantime.
pub const ReadFn = ?*const anyopaque;

/// A magnifiable full-resolution image behind the gray8 sub-rect seam
/// (zoom_source_t): the reader function, its context, and the extent.
pub const Source = struct {
    read: ReadFn = null,
    ctx: ?*anyopaque = null,
    width: u32 = 0,
    height: u32 = 0,
};

/// Caller-owned composite buffers, the module's entire memory footprint
/// (zoom_scratch_t): one source row, one destination strip in gray8, and
/// the same strip packed to 4 bpp.
pub const Scratch = struct {
    row: ?[*]u8 = null,
    row_cap: u32 = 0,
    strip: ?[*]u8 = null,
    strip_cap: u32 = 0,
    packed_buf: ?[*]u8 = null,
    packed_cap: u32 = 0,
};

/// The waveform a owed flush should use (zoom_refresh_t).
pub const Refresh = enum(u8) {
    /// Interactive burst: panel A2 (bi-level, fast).
    fast = 0,
    /// Settled: panel GC16 (all 16 levels, slow).
    quality = 1,
};

/// The waveform scheduling policy (zoom_policy_t).
pub const Policy = enum(u8) {
    /// Fast during a burst, quality once settled.
    responsive = 0,
    /// Always quality; no bi-level frame is shown.
    quality = 1,
};

/// Live viewport: where it is looking, how magnified, and what it owes the
/// panel (zoom_view_t). Opaque by convention: read it through the accessor
/// functions, never write the fields directly. The two anchor fields are in
/// magnified-plane coordinates, so anchor_x + c is the plane column shown
/// at destination column c and (anchor_x + c) / scale is the source column.
pub const View = struct {
    src: Source = .{},
    scratch: Scratch = .{},
    dst: Rect = .{},
    anchor_x: i32 = 0,
    anchor_y: i32 = 0,
    last_input_ms: u32 = 0,
    settle_ms: u16 = 0,
    strip_rows: u16 = 0,
    scale: u8 = 0,
    scale_max: u8 = 0,
    policy: Policy = .responsive,
    pending_kind: Refresh = .fast,
    pending: bool = false,
    settle_armed: bool = false,
    active: bool = false,
    /// Tone curve adopted from the config (null = flat even palette); the
    /// typed ra8_gfx_tone_map_t seam lands with the view ABI slice.
    tone: ?*const anyopaque = null,
};

/// Re-establish the clamp invariant on an open view (internal_reclamp). The
/// one place every mutation site re-clamps through: an open, a scale
/// change, a pan, a rebind onto a differently-sized page. `v.scale` is
/// already the scale the anchors should be clamped for.
pub fn reclamp(v: *View) void {
    const scale: i32 = v.scale;
    v.anchor_x = clampAnchor(v.anchor_x, @as(i32, @intCast(v.src.width)) * scale, v.dst.w);
    v.anchor_y = clampAnchor(v.anchor_y, @as(i32, @intCast(v.src.height)) * scale, v.dst.h);
}

/// Record an interactive viewport change and schedule the flush it owes
/// (internal_touch). The one place the dirty flag and the waveform class
/// are set: under the responsive policy an interactive change owes a fast
/// (bi-level) flush and arms the settle timer; under the quality policy it
/// owes a full 16-level flush and arms nothing.
pub fn touch(v: *View, now_ms: u32) void {
    const responsive = v.policy == .responsive;
    v.last_input_ms = now_ms;
    v.pending = true;
    v.pending_kind = if (responsive) .fast else .quality;
    v.settle_armed = responsive;
}

/// Close a view and drop everything it owes (zoom_view_close). A closed
/// view keeps its geometry so the caller can still read where it was.
pub fn close(v: *View) void {
    v.active = false;
    v.pending = false;
    v.settle_armed = false;
}

/// Whether the view is open (zoom_view_active).
pub fn isActive(v: *const View) bool {
    return v.active;
}

test "reclamp bounds a big image and centres a small one" {
    // 4x of a 16 px source is a 64 px plane against a 32 px viewport:
    // anchors are held inside [0, 64 - 32].
    var v = View{ .src = .{ .width = 16, .height = 16 }, .dst = .{ .w = 32, .h = 32 }, .scale = 4 };
    v.anchor_x = 99;
    v.anchor_y = -9;
    reclamp(&v);
    try std.testing.expectEqual(@as(i32, 32), v.anchor_x);
    try std.testing.expectEqual(@as(i32, 0), v.anchor_y);
    // Smaller than the viewport: centred, anchors negative (letterbox).
    var s = View{ .src = .{ .width = 8, .height = 8 }, .dst = .{ .w = 32, .h = 32 }, .scale = 2 };
    s.anchor_x = 5;
    s.anchor_y = 5;
    reclamp(&s);
    try std.testing.expectEqual(@as(i32, -8), s.anchor_x);
    try std.testing.expectEqual(@as(i32, -8), s.anchor_y);
}

test "touch under responsive arms settle and owes fast" {
    var v = View{ .policy = .responsive };
    touch(&v, 1000);
    try std.testing.expectEqual(@as(u32, 1000), v.last_input_ms);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(Refresh.fast, v.pending_kind);
    try std.testing.expect(v.settle_armed);
}

test "touch under quality owes a full flush and arms nothing" {
    var v = View{ .policy = .quality };
    touch(&v, 1000);
    try std.testing.expect(v.pending);
    try std.testing.expectEqual(Refresh.quality, v.pending_kind);
    try std.testing.expect(!v.settle_armed);
}

test "close drops everything the view owes" {
    var v = View{ .active = true, .pending = true, .settle_armed = true };
    try std.testing.expect(isActive(&v));
    close(&v);
    try std.testing.expect(!isActive(&v));
    try std.testing.expect(!v.pending);
    try std.testing.expect(!v.settle_armed);
}
