// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

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
const std = @import("std");

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
pub fn expandRow(v: *View, ax: *const Axis, drow: [*]u8) void {
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
pub fn fillRow(v: *View, ax: *const Axis, plane_y: i32, drow: [*]u8, cached_sy: *i32) Err {
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
pub fn fillStrip(v: *View, ax: *const Axis, top: i32, rows: i32, cached_sy: *i32) Err {
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
pub fn emitStrip(v: *View, ax: *const Axis, top: i32, rows: i32, cached_sy: *i32) Err {
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
pub fn renderReady(v: ?*const View) Err {
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
