//! Strip composite for the tap-to-zoom viewer (RA8FW-1260): magnify,
//! re-dither, blit.
//!
//! Port of zoom_render.c. The render half of zoom.h. It never holds the
//! visible window: a 1024x600 gray8 window would be 600 KiB and cannot be
//! allocated (NASA P10 Rule 3), so the viewport is composited in horizontal
//! strips of at most `View.strip_rows` destination rows and the resident cost
//! is `O(dst.w * strip_rows)`.
//!
//! Each strip goes through the same three steps:
//!
//!  1. **Magnify.** Nearest-neighbour replication straight into the
//!     destination grid, one source row read per *distinct* source row
//!     (consecutive destination rows at zoom `n` share a source row, and the
//!     row buffer is reused rather than re-faulted). Uncovered pixels are
//!     filled with `zoom.bg_gray`, so the caller never pre-clears the
//!     viewport.
//!  2. **Re-dither at the destination resolution**, not the source's. The
//!     source is dithered after magnification so the grain stays one panel
//!     pixel wide at every zoom. The mask phase is the *magnified-plane*
//!     coordinate, so a given image pixel keeps its threshold as the viewport
//!     moves -- panning does not re-roll the grain (see the zoom.h docblock).
//!  3. **Blit** the packed 4 bpp strip through `ra8_gfx_blit_gray4_zoom` at
//!     native scale. The magnification has already happened, so this call is
//!     a nibble-unpack and clipped store -- the same tested primitive the
//!     shelf loupe uses, rather than a second pixel path.
//!
//! Every operation is integer arithmetic over source bytes and a const mask,
//! so a framebuffer hash of the result is identical on the unit-test host, in
//! ra8_emulator and on silicon.
//!
//! External symbols: `ra8_gfx_dither_gray8_to_gray4_tone` and
//! `ra8_gfx_blit_gray4_zoom` (ra8_gfx.h / ra8_gfx_dither.h), supplied by the
//! ra8_gfx library at the final link, and `ra8_log_emit_error`
//! (ra8_log.h's backend entry), supplied by ra8_core_hal. The standalone test
//! build installs silent/stub versions of all three; the firmware build links
//! the real definitions.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const builtin = @import("builtin");

pub const zoom = @import("zoom.zig");

const Err = zoom.Err;
const View = zoom.View;
const Axis = zoom.Axis;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;
extern fn ra8_gfx_dither_gray8_to_gray4_tone(map: ?*const ToneMap, src: [*]const u8, w: i32, h: i32, origin_x: i32, origin_y: i32, out: [*]u8, out_cap: u32, out_size: *u32) callconv(.c) Err;
extern fn ra8_gfx_blit_gray4_zoom(src: [*]const u8, src_w: i32, src_h: i32, sx: i32, sy: i32, sw: i32, sh: i32, zoom: i32, dst_x: i32, dst_y: i32) callconv(.c) Err;

/// Mirror of ra8_gfx_tone_map_t (libs/ra8_gfx/inc/ra8_gfx_tone.h): the
/// prepared tone curve the dither hot loop reads. `level[v]` is the panel
/// level at or below gray8 sample `v`; `up_threshold[v]` is the exclusive
/// blue-noise round-up threshold. Layout parity is pinned at compile time
/// because the render hands a borrowed caller pointer to the C dither.
pub const ToneMap = extern struct {
    level: [256]u8,
    up_threshold: [256]u16,
};

comptime {
    if (@sizeOf(ToneMap) != 256 * 1 + 256 * 2) {
        @compileError("ToneMap layout parity with ra8_gfx_tone_map_t broken (768 B)");
    }
    if (@offsetOf(ToneMap, "up_threshold") != 256) {
        @compileError("ToneMap up_threshold must sit at byte 256 like the C struct");
    }
}

/// Component tag for this module's log lines (zoom_render.c s_tag).
const s_tag: [*:0]const u8 = "zoom_render";

fn emitLog(msg: [*:0]const u8) void {
    if (zoom.log_hook) |f| {
        f(std.mem.span(msg));
    } else {
        ra8_log_emit_error(s_tag, msg);
    }
}

/// One log line per failure the C module emits through RA8_CHECK_* /
/// RA8_RETURN_ON_ERROR (zoom_render.c), routed through the C core's
/// ra8_log_emit_error under the module's tag unless a hook overrides.
fn logErr(comptime msg: [:0]const u8) void {
    emitLog(msg);
}

/// The one runtime-valued log line (zoom_render.c's ra8_log_error_val at the
/// source-read failure): the message carries the faulting source row, which
/// on a paged source IS the diagnostic.
fn logErrVal(comptime prefix: [:0]const u8, value: u32) void {
    var buf: [96]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, prefix ++ "{d}", .{value}) catch {
        logErr(prefix);
        return;
    };
    buf[msg.len] = 0;
    emitLog(@ptrCast(msg.ptr));
}

/// Fixed arguments of the per-strip packed-gray4 blit (zoom_blit_t). The
/// strip is already at destination resolution when it reaches
/// ra8_gfx_blit_gray4_zoom, so the blit magnifies by `blit_native` (1) and
/// takes the whole strip from its origin. Naming them keeps the call
/// self-describing and keeps the "no magic numbers" rule honest at the one
/// place a reader would otherwise have to count arguments.
const blit_origin: i32 = 0;
const blit_native: i32 = 1;

/// Replicate one source row across the covered destination columns
/// (internal_expand_row). The horizontal half of the nearest-neighbour
/// magnify: destination offset `d` shows plane column `anchor_x + d`, hence
/// source column `(anchor_x + d) / scale`, which is an index into the row
/// buffer once the axis' own `s0` is subtracted. Columns outside
/// `[ax.d0, ax.d1)` are letterbox and are left at the background fill the
/// caller wrote.
fn expandRow(v: *View, ax: *const Axis, drow: [*]u8) void {
    const scale: i32 = v.scale;
    // The whole index calculation stays signed and the only conversion is of
    // the axis' own simple `s0` -- a cast of the composite `(anchor + d) /
    // scale` to an unsigned type would change essential type mid-expression
    // (MISRA 10.8) for no benefit: every value here is non-negative by
    // construction.
    const base: i32 = @intCast(ax.s0);
    var d: i32 = ax.d0;
    while (d < ax.d1) : (d += 1) {
        const si: i32 = @divTrunc(v.anchor_x + d, scale) - base;
        drow[@intCast(d)] = v.scratch.row.?[@intCast(si)];
    }
}

/// Composite one destination row: decide coverage, fault, replicate
/// (internal_fill_row). A row above the image, below it, or on an axis with
/// no horizontal coverage is letterbox and returns having written nothing --
/// the caller has already filled it with the background. A covered row reads
/// its source row only when it differs from the one already in the row
/// buffer, which at zoom `n` is once per `n` destination rows and never twice
/// across a strip boundary, since `cached_sy` carries over.
fn fillRow(v: *View, ax: *const Axis, plane_y: i32, drow: [*]u8, cached_sy: *i32) Err {
    const scale: i32 = v.scale;
    const height: i32 = @intCast(v.src.height);
    const sy: i32 = if (plane_y >= 0) @divTrunc(plane_y, scale) else -1;
    // Decision: this destination row shows image only if the plane row lies
    // inside the image. The horizontal span is NOT re-tested here -- it is
    // constant across the frame and fillStrip settles it once, which keeps
    // this decision to two conditions that are each independently reachable
    // rather than three where one never varies.
    const covered = (sy >= 0) and (sy < height);
    if (!covered) {
        return .ok;
    }
    if (sy != cached_sy.*) {
        const rc = v.src.read.?(v.src.ctx, ax.s0, @intCast(sy), ax.count, 1, v.scratch.row.?, ax.count);
        if (rc != .ok) {
            // Name the row. The caller only learns that the frame was
            // abandoned, and on a paged source which source row faulted IS the
            // diagnostic.
            logErrVal("source read failed at row ", @intCast(sy));
            cached_sy.* = -1;
            return rc;
        }
        cached_sy.* = sy;
    }
    expandRow(v, ax, drow);
    return .ok;
}

/// Build one gray8 destination strip: background fill, then per-row
/// composite (internal_fill_strip). The background fill is unconditional and
/// covers the whole strip in one pass, so a letterbox row needs no second
/// visit and the caller never has to pre-clear the viewport. The
/// horizontal-coverage test is settled here, once per strip, rather than per
/// row: it is constant across the frame, and a zero-width row would violate
/// the source seam's `w > 0` contract if it ever reached zoom_read_fn.
/// Everything else is fillRow's decision.
fn fillStrip(v: *View, ax: *const Axis, top: i32, rows: i32, cached_sy: *i32) Err {
    const stride: usize = @intCast(v.dst.w);
    const strip = v.scratch.strip.?;
    @memset(strip[0 .. @as(usize, @intCast(rows)) * stride], zoom.bg_gray);
    if (ax.count == 0) {
        // The image misses the viewport horizontally, so every row is
        // letterbox and the background fill above is the whole answer.
        return .ok;
    }
    var r: i32 = 0;
    while (r < rows) : (r += 1) {
        const drow = strip + @as(usize, @intCast(r)) * stride;
        const rc = fillRow(v, ax, v.anchor_y + top + r, drow, cached_sy);
        if (rc != .ok) {
            logErr("strip row");
            return rc;
        }
    }
    return .ok;
}

/// One strip, end to end: magnify, re-dither at plane phase, blit
/// (internal_emit_strip). The three-step pipeline the file docblock
/// describes, kept together so viewRender is only the strip loop. The dither
/// phase is the magnified-plane origin of this strip, which is what makes the
/// grain belong to the image rather than to the panel.
fn emitStrip(v: *View, ax: *const Axis, top: i32, rows: i32, cached_sy: *i32) Err {
    const fill_err = fillStrip(v, ax, top, rows, cached_sy);
    if (fill_err != .ok) {
        logErr("strip fill");
        return fill_err;
    }
    var packed_len: u32 = 0;
    const dither_err = ra8_gfx_dither_gray8_to_gray4_tone(
        @as(?*const ToneMap, @ptrCast(@alignCast(v.tone))),
        v.scratch.strip.?,
        v.dst.w,
        rows,
        v.anchor_x,
        v.anchor_y + top,
        v.scratch.packed_buf.?,
        v.scratch.packed_cap,
        &packed_len,
    );
    if (dither_err != .ok) {
        logErr("strip dither");
        return dither_err;
    }
    return ra8_gfx_blit_gray4_zoom(
        v.scratch.packed_buf.?,
        v.dst.w,
        rows,
        blit_origin,
        blit_origin,
        v.dst.w,
        rows,
        blit_native,
        v.dst.x,
        v.dst.y + top,
    );
}

/// Whether a view is in a state that can be composited at all
/// (internal_render_ready). Split out of viewRender so the render body is the
/// strip loop and nothing else. The strip-height check is not defensive
/// padding: it is the loop bound, and a zero would make the strip loop fail
/// to advance (NASA P10 Rule 2).
fn renderReady(v: ?*const View) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (v.?.src.read == null) {
        logErr("view source seam must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("render on a closed view");
        return .invalid_state;
    }
    if (v.?.strip_rows == 0) {
        logErr("view has no strip height; it was not opened properly");
        return .invalid_state;
    }
    return .ok;
}

/// Composite the whole viewport one strip at a time (zoom_view_render).
pub fn viewRender(v: ?*View) Err {
    const ready = renderReady(v);
    if (ready != .ok) {
        logErr("render preconditions");
        return ready;
    }
    const view = v.?;
    const ax = zoom.axis(view.anchor_x, view.dst.w, @as(i32, view.scale), @as(i32, @intCast(view.src.width)));

    const rows_max: i32 = view.strip_rows;
    var cached_sy: i32 = -1;
    var top: i32 = 0;
    while (top < view.dst.h) : (top += rows_max) {
        var rows: i32 = view.dst.h - top;
        if (rows > rows_max) {
            rows = rows_max;
        }
        const emit_err = emitStrip(view, &ax, top, rows, &cached_sy);
        if (emit_err != .ok) {
            logErr("strip emit");
            return emit_err;
        }
    }
    return .ok;
}

// ---------------------------------------------------------------------------
// The standalone test build: silent log, stub dither and stub blit. The
// firmware build links the real ra8_gfx definitions instead.
// ---------------------------------------------------------------------------

comptime {
    if (builtin.is_test) {
        @export(&testDitherStub, .{ .name = "ra8_gfx_dither_gray8_to_gray4_tone" });
        @export(&testBlitStub, .{ .name = "ra8_gfx_blit_gray4_zoom" });
    }
}

/// Controllable stub failures and captures for the test suite.
pub var test_dither_err: Err = .ok;
pub var test_blit_err: Err = .ok;
pub var test_dither_calls: u32 = 0;
pub var test_blit_calls: u32 = 0;
pub const DitherArgs = struct { w: i32 = 0, h: i32 = 0, origin_x: i32 = 0, origin_y: i32 = 0, out_cap: u32 = 0 };
pub const BlitArgs = struct { src_w: i32 = 0, src_h: i32 = 0, sw: i32 = 0, sh: i32 = 0, zoom: i32 = 0, dst_x: i32 = 0, dst_y: i32 = 0 };
pub var test_dither_args: DitherArgs = .{};
pub var test_blit_args: BlitArgs = .{};

fn testDitherStub(map: ?*const ToneMap, src: [*]const u8, w: i32, h: i32, origin_x: i32, origin_y: i32, out: [*]u8, out_cap: u32, out_size: *u32) callconv(.c) Err {
    _ = map;
    test_dither_calls += 1;
    test_dither_args = .{ .w = w, .h = h, .origin_x = origin_x, .origin_y = origin_y, .out_cap = out_cap };
    // Pack gray8 nibble-high/low, the trivially checkable stand-in for the
    // blue-noise dither the real ra8_gfx runs.
    const n: usize = @intCast(w * h);
    var i: usize = 0;
    while (i < (n + 1) / 2) : (i += 1) {
        const hi: u8 = src[2 * i] >> 4;
        const lo: u8 = if (2 * i + 1 < n) src[2 * i + 1] >> 4 else 0;
        if (i < out_cap) {
            out[i] = (hi << 4) | lo;
        }
    }
    out_size.* = @intCast((n + 1) / 2);
    return test_dither_err;
}

fn testBlitStub(src: [*]const u8, src_w: i32, src_h: i32, sx: i32, sy: i32, sw: i32, sh: i32, zoom_arg: i32, dst_x: i32, dst_y: i32) callconv(.c) Err {
    _ = src;
    _ = sx;
    _ = sy;
    test_blit_calls += 1;
    test_blit_args = .{ .src_w = src_w, .src_h = src_h, .sw = sw, .sh = sh, .zoom = zoom_arg, .dst_x = dst_x, .dst_y = dst_y };
    return test_blit_err;
}

// ---------------------------------------------------------------------------
// Implementation suite. The C host suite (tests/src/test_zoom.c) drives the
// same pipeline end to end over the C ABI against the real ra8_gfx; these
// tests pin the strip decisions themselves against stubs.
// ---------------------------------------------------------------------------

fn testReadFill(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    if (zoom_render_test_read_fail) {
        return .no_mem;
    }
    const img: *const [64]u8 = @ptrCast(@alignCast(ctx));
    var r: u32 = 0;
    while (r < h) : (r += 1) {
        var c: u32 = 0;
        while (c < w) : (c += 1) {
            out[r * out_stride + c] = img[(y + r) * 8 + (x + c)];
        }
    }
    return .ok;
}

var zoom_render_test_read_fail: bool = false;

test "renderReady rejects each broken state in the C order" {
    var v = View{};
    try std.testing.expectEqual(Err.null_ptr, renderReady(null));
    try std.testing.expectEqual(Err.null_ptr, renderReady(&v));
    v.src.read = testReadFill;
    try std.testing.expectEqual(Err.invalid_state, renderReady(&v));
    v.active = true;
    try std.testing.expectEqual(Err.invalid_state, renderReady(&v));
    v.strip_rows = 2;
    try std.testing.expectEqual(Err.ok, renderReady(&v));
}

test "expandRow samples source columns and leaves letterbox alone" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .row_cap = 4 },
        .scale = 2,
        .anchor_x = 0,
        .active = true,
        .strip_rows = 2,
    };
    // A 4-px source at x2: the plane is 8 px wide against an 8-px viewport at
    // anchor 0, so every destination column is covered and shows source
    // column d/2.
    const ax = zoom.axis(0, 8, 2, 4);
    var drow: [8]u8 = undefined;
    var i: usize = 0;
    while (i < drow.len) : (i += 1) {
        drow[i] = 0xEE;
    }
    // The row buffer holds source columns [0, 4): the test reader's row 0.
    for (&row, 0..) |*rp, ri| rp.* = @intCast(ri);
    expandRow(&v, &ax, &drow);
    for (drow, 0..) |b, d| {
        try std.testing.expectEqual(@as(u8, @intCast(d / 2)), b);
    }
    // Letterbox on the left: anchor -4 puts d0 at 4, so columns 0..3 keep
    // the background the caller wrote.
    const ax_lb = zoom.axis(-4, 8, 2, 4);
    v.anchor_x = -4;
    v.scratch.row.?[@intCast(ax_lb.s0)] = 9; // source col 0 of the covered span
    var drow2: [8]u8 = undefined;
    i = 0;
    while (i < drow2.len) : (i += 1) {
        drow2[i] = 0xEE;
    }
    expandRow(&v, &ax_lb, &drow2);
    try std.testing.expectEqual(@as(u8, 0xEE), drow2[0]);
    try std.testing.expectEqual(@as(u8, 0xEE), drow2[3]);
    try std.testing.expectEqual(@as(u8, 9), drow2[4]);
}

test "fillRow faults only on a new source row and resets on failure" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .row_cap = 4 },
        .scale = 2,
        .anchor_x = 0,
        .active = true,
        .strip_rows = 2,
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var drow: [8]u8 = undefined;
    var cached: i32 = -1;
    // Plane rows 0 and 1 share source row 0: the second call must not read.
    try std.testing.expectEqual(Err.ok, fillRow(&v, &ax, 0, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    try std.testing.expectEqual(Err.ok, fillRow(&v, &ax, 1, &drow, &cached));
    // Above and below the image are letterbox and reset nothing.
    try std.testing.expectEqual(Err.ok, fillRow(&v, &ax, -1, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    try std.testing.expectEqual(Err.ok, fillRow(&v, &ax, 8, &drow, &cached));
    try std.testing.expectEqual(@as(i32, 0), cached);
    // A failing read propagates verbatim and invalidates the row cache.
    zoom_render_test_read_fail = true;
    defer zoom_render_test_read_fail = false;
    try std.testing.expectEqual(Err.no_mem, fillRow(&v, &ax, 2, &drow, &cached));
    try std.testing.expectEqual(@as(i32, -1), cached);
}

test "fillStrip fills letterbox first and composites covered rows" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .row_cap = 4, .strip_cap = 16 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var cached: i32 = -1;
    try std.testing.expectEqual(Err.ok, fillStrip(&v, &ax, 0, 2, &cached));
    // Strip rows 0..1 show plane rows 0..1, i.e. source row 0 twice: every
    // covered column d shows source column d/2 of row 0 (img[d/2]).
    var c: usize = 0;
    while (c < 8) : (c += 1) {
        try std.testing.expectEqual(@as(u8, @intCast(c / 2)), strip[c]);
        try std.testing.expectEqual(@as(u8, @intCast(c / 2)), strip[8 + c]);
    }
    // A horizontally uncovered axis: the whole strip stays background and no
    // source row is faulted.
    var img2: [64]u8 = undefined;
    for (&img2) |*b| b.* = 0xAA;
    var row2: [4]u8 = undefined;
    var strip2: [16]u8 = undefined;
    var v2 = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img2), .width = 4, .height = 4 },
        .scratch = .{ .row = row2[0..].ptr, .strip = strip2[0..].ptr, .row_cap = 4, .strip_cap = 16 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 100,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    var cached2: i32 = -1;
    try std.testing.expectEqual(Err.ok, fillStrip(&v2, &zoom.axis(100, 8, 2, 4), 0, 2, &cached2));
    for (strip2) |b| try std.testing.expectEqual(zoom.bg_gray, b);
    try std.testing.expectEqual(@as(i32, -1), cached2);
}

test "emitStrip dithers at plane phase and blits at native scale" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    var packed_buf: [8]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var tone = ToneMap{ .level = undefined, .up_threshold = undefined };
    var v = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .packed_buf = packed_buf[0..].ptr, .row_cap = 4, .strip_cap = 16, .packed_cap = 8 },
        .dst = .{ .x = 3, .y = 4, .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
        .tone = @ptrCast(&tone),
    };
    const ax = zoom.axis(0, 8, 2, 4);
    var cached: i32 = -1;
    test_dither_calls = 0;
    test_blit_calls = 0;
    try std.testing.expectEqual(Err.ok, emitStrip(&v, &ax, 6, 2, &cached));
    try std.testing.expectEqual(@as(u32, 1), test_dither_calls);
    try std.testing.expectEqual(@as(i32, 8), test_dither_args.w);
    try std.testing.expectEqual(@as(i32, 2), test_dither_args.h);
    // The dither phase is the magnified-plane origin of the strip.
    try std.testing.expectEqual(@as(i32, 0), test_dither_args.origin_x);
    try std.testing.expectEqual(@as(i32, 6), test_dither_args.origin_y);
    try std.testing.expectEqual(@as(u32, 1), test_blit_calls);
    try std.testing.expectEqual(@as(i32, 8), test_blit_args.sw);
    try std.testing.expectEqual(@as(i32, 2), test_blit_args.sh);
    try std.testing.expectEqual(@as(i32, 1), test_blit_args.zoom); // native
    try std.testing.expectEqual(@as(i32, 3), test_blit_args.dst_x);
    try std.testing.expectEqual(@as(i32, 10), test_blit_args.dst_y);
    // A failing blit surfaces the gfx error verbatim (hw_error when the
    // framebuffer binding is missing).
    test_blit_err = .hw_error;
    defer test_blit_err = .ok;
    try std.testing.expectEqual(Err.hw_error, emitStrip(&v, &ax, 6, 2, &cached));
}

test "viewRender strips the viewport and stops on a failing strip" {
    var img: [64]u8 = undefined;
    var row: [4]u8 = undefined;
    var strip: [16]u8 = undefined;
    var packed_buf: [8]u8 = undefined;
    for (&img, 0..) |*p, i| p.* = @intCast(i);
    var v = View{
        .src = .{ .read = testReadFill, .ctx = @ptrCast(&img), .width = 4, .height = 4 },
        .scratch = .{ .row = row[0..].ptr, .strip = strip[0..].ptr, .packed_buf = packed_buf[0..].ptr, .row_cap = 4, .strip_cap = 16, .packed_cap = 8 },
        .dst = .{ .w = 8, .h = 8 },
        .scale = 2,
        .anchor_x = 0,
        .anchor_y = 0,
        .active = true,
        .strip_rows = 2,
    };
    test_dither_calls = 0;
    test_blit_calls = 0;
    try std.testing.expectEqual(Err.ok, viewRender(&v));
    // Four strips of two rows cover the 8-row viewport.
    try std.testing.expectEqual(@as(u32, 4), test_blit_calls);
    try std.testing.expectEqual(@as(u32, 4), test_dither_calls);
    // The last strip lands at destination row 6.
    try std.testing.expectEqual(@as(i32, 6), test_blit_args.dst_y);
    // A source fault abandons the frame at the strip boundary: strips before
    // the fault are on the framebuffer, nothing after is blitted.
    test_dither_calls = 0;
    test_blit_calls = 0;
    zoom_render_test_read_fail = true;
    defer zoom_render_test_read_fail = false;
    try std.testing.expectEqual(Err.no_mem, viewRender(&v));
    try std.testing.expectEqual(@as(u32, 0), test_blit_calls);
    // A null view and a closed view never reach the strip loop.
    try std.testing.expectEqual(Err.null_ptr, viewRender(null));
    v.active = false;
    try std.testing.expectEqual(Err.invalid_state, viewRender(&v));
}
