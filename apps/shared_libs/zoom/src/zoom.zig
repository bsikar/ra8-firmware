// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Viewport state machine for the tap-to-zoom image viewer (RA8FW-1253).
//!
//! Port of the pure geometry core of zoom.c: the magnified-plane axis window,
//! the dimension bound, the saturating anchor add and the anchor clamp. Pure
//! integer arithmetic with no MMIO and no allocation, so it behaves identically
//! on the unit-test host, in ra8_emulator and on silicon. The strip composite
//! stays in zoom_render.zig (RA8FW-1260) and the zoom_view_* C ABI seams live
//! in zoom_abi.zig.

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
pub const Axis = extern struct {
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

/// One axis-aligned rectangle in framebuffer pixel coordinates
/// (ra8_ui_rect_t): inclusive on the top-left corner, exclusive on the
/// bottom-right, so w = h = 0 contains no points.
pub const Rect = extern struct {
    x: i32 = 0,
    y: i32 = 0,
    w: i32 = 0,
    h: i32 = 0,
};

/// The gray8 sub-rectangle reader behind a bound source (zoom_read_fn):
/// the typed error seam, carrying the exact C signature.
pub const ReadFn = ?*const fn (?*anyopaque, u32, u32, u32, u32, [*]u8, u32) callconv(.c) Err;

/// A magnifiable full-resolution image behind the gray8 sub-rect seam
/// (zoom_source_t): the reader function, its context, and the extent.
pub const Source = extern struct {
    read: ReadFn = null,
    ctx: ?*anyopaque = null,
    width: u32 = 0,
    height: u32 = 0,
};

/// Caller-owned composite buffers, the module's entire memory footprint
/// (zoom_scratch_t): one source row, one destination strip in gray8, and
/// the same strip packed to 4 bpp.
pub const Scratch = extern struct {
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
pub const View = extern struct {
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

/// The open request (zoom_view_cfg_t): what the caller wants at open
/// time. Carries the opening focus and scale the live view is derived from;
/// the view keeps only what survives the open.
pub const Cfg = extern struct {
    src: Source = .{},
    scratch: Scratch = .{},
    dst: Rect = .{},
    scale: u8 = 0,
    scale_max: u8 = 0,
    policy: Policy = .responsive,
    settle_ms: u16 = 0,
    focus_x: i32 = 0,
    focus_y: i32 = 0,
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

/// C ABI parity error codes (ra8_err_t): the fixture pins ok==0,
/// invalid_arg==0x103, no_mem==0x102, null_ptr==0x504. The invalid_state
/// code arrives with the open/rebind/invalidate slice.
pub const Err = enum(u32) {
    ok = 0x000,
    no_mem = 0x102,
    invalid_arg = 0x103,
    null_ptr = 0x504,
    invalid_state = 0x104,
    out_of_range = 0x208,
    invalid_size = 0x105, // k_ra8_err_invalid_size: the tiles adapter surfaces it (tile geometry mismatch).
    not_supported = 0x107, // k_ra8_err_not_supported: adapters surface it (zoom_book SVG reject).
    hw_error = 0x204, // k_ra8_err_hw_error: the render pipeline surfaces it (gfx blit/dither).
};

/// The ra8_log_error macro's backend entry (ra8_log.h): tag + free-form
/// message. Declared here because the port's include closure does not reach
/// ra8_log.h's macro surface; the real build links the C core's definition.
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;

/// Component tag for the module's log lines (zoom.c s_tag).
const s_tag: [*:0]const u8 = "zoom";

/// Caller-installed log override, null by default. Installed, the hook
/// receives every diagnostic line instead of the C core's sink; the
/// standalone test build installs nothing and the extern stays silent.
pub const LogFn = *const fn (msg: []const u8) void;
pub var log_hook: ?LogFn = null;

/// One log line per rejected configuration, the same messages the C module
/// emits (zoom.c RA8_CHECK_* calls), routed through the C core's
/// ra8_log_emit_error under the module's tag unless a hook overrides.
pub fn logErr(comptime msg: [:0]const u8) void {
    if (log_hook) |f| {
        f(msg);
    } else {
        ra8_log_emit_error(s_tag, msg.ptr);
    }
}

/// The view's required pointers must all be live (internal_cfg_ptrs_ok).
pub fn cfgPtrsOk(cfg: *const Cfg) Err {
    if (cfg.src.read == null) {
        logErr("source read seam must not be nullptr");
        return .null_ptr;
    }
    if (cfg.scratch.row == null) {
        logErr("scratch.row must not be nullptr");
        return .null_ptr;
    }
    if (cfg.scratch.strip == null) {
        logErr("scratch.strip must not be nullptr");
        return .null_ptr;
    }
    if (cfg.scratch.packed_buf == null) {
        logErr("scratch.packed must not be nullptr");
        return .null_ptr;
    }
    return .ok;
}

/// Viewport and source extents must be usable (internal_cfg_extent_ok).
pub fn cfgExtentOk(cfg: *const Cfg) Err {
    if (cfg.dst.w <= 0 or cfg.dst.h <= 0) {
        logErr("viewport must have positive width and height");
        return .invalid_arg;
    }
    if (!dimOk(cfg.src.width) or !dimOk(cfg.src.height)) {
        logErr("source extent is zero or exceeds k_zoom_dim_max");
        return .invalid_arg;
    }
    return .ok;
}

/// Scratch buffers must hold one composited strip at the viewport width
/// (internal_cfg_scratch_ok); writes back the strip row count.
pub fn cfgScratchOk(cfg: *const Cfg, out_strip_rows: *u16) Err {
    const width: u32 = @intCast(cfg.dst.w);
    if (cfg.scratch.row_cap < width or cfg.scratch.strip_cap < width) {
        logErr("scratch row/strip buffer is narrower than the viewport");
        return .no_mem;
    }
    var rows: u32 = cfg.scratch.strip_cap / width;
    if (rows > strip_rows_max) {
        rows = strip_rows_max;
    }
    if (rows > @as(u32, @intCast(cfg.dst.h))) {
        rows = @intCast(cfg.dst.h);
    }
    if (cfg.scratch.packed_cap < (width * rows + 1) / 2) {
        logErr("scratch packed buffer cannot hold one dithered strip");
        return .no_mem;
    }
    out_strip_rows.* = @intCast(rows);
    return .ok;
}

/// The scale ladder: zero-valued config entries lift to the defaults, then
/// the pair must sit inside the engine ladder (internal_cfg_ladder_ok).
pub fn cfgLadderOk(cfg: *const Cfg, out_scale: *u8, out_ceiling: *u8) Err {
    const ladder_max: u8 = @intCast(scale_max);
    const ladder_min: u8 = @intCast(scale_min);
    const ceiling: u8 = if (cfg.scale_max == 0) ladder_max else cfg.scale_max;
    const opening: u8 = if (cfg.scale == 0) ladder_min else cfg.scale;
    if (ceiling > ladder_max) {
        logErr("scale_max is outside the engine ladder");
        return .invalid_arg;
    }
    if (opening > ceiling) {
        logErr("opening scale is outside the configured ladder");
        return .invalid_arg;
    }
    out_scale.* = opening;
    out_ceiling.* = ceiling;
    return .ok;
}

/// Bind a full-resolution source descriptor (zoom_source_init): the reader
/// seam, its context, and an extent inside the engine cap.
pub fn sourceInit(out: ?*Source, read: ReadFn, ctx: ?*anyopaque, width: u32, height: u32) Err {
    if (out == null) {
        logErr("source out must not be nullptr");
        return .null_ptr;
    }
    if (read == null) {
        logErr("source read seam must not be nullptr");
        return .null_ptr;
    }
    if (!dimOk(width) or !dimOk(height)) {
        logErr("source extent is zero or exceeds k_zoom_dim_max");
        return .invalid_arg;
    }
    out.?.read = read;
    out.?.ctx = ctx;
    out.?.width = width;
    out.?.height = height;
    return .ok;
}

/// Default input-silence settle window (k_zoom_settle_ms_default).
pub const settle_ms_default: u16 = 350;

/// The full open gate: pointers, extents, scratch capacity, ladder
/// (internal_cfg_ok), in the C module's order.
fn cfgOk(cfg: *const Cfg, out_strip_rows: *u16, out_scale: *u8, out_ceiling: *u8) Err {
    const e1 = cfgPtrsOk(cfg);
    if (e1 != .ok) return e1;
    const e2 = cfgExtentOk(cfg);
    if (e2 != .ok) return e2;
    const e3 = cfgScratchOk(cfg, out_strip_rows);
    if (e3 != .ok) return e3;
    return cfgLadderOk(cfg, out_scale, out_ceiling);
}

/// Copy a validated config into a live view and arm its first flush
/// (internal_view_adopt): anchors open centred on the focus point.
fn viewAdopt(v: *View, cfg: *const Cfg, opening: u8, ceiling: u8, strip_rows: u16) void {
    v.src = cfg.src;
    v.scratch = cfg.scratch;
    v.dst = cfg.dst;
    v.scale = opening;
    v.scale_max = ceiling;
    v.tone = cfg.tone;
    v.policy = cfg.policy;
    v.settle_ms = if (cfg.settle_ms == 0) settle_ms_default else cfg.settle_ms;
    v.strip_rows = strip_rows;
    v.anchor_x = cfg.focus_x * @as(i32, opening) - @divTrunc(cfg.dst.w, 2);
    v.anchor_y = cfg.focus_y * @as(i32, opening) - @divTrunc(cfg.dst.h, 2);
    v.last_input_ms = 0;
    v.settle_armed = false;
    v.pending = true;
    v.pending_kind = .quality;
    v.active = true;
    reclamp(v);
}

/// Open a view from a validated config (zoom_view_open).
pub fn viewOpen(v: ?*View, cfg: ?*const Cfg) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (cfg == null) {
        logErr("cfg must not be nullptr");
        return .null_ptr;
    }
    v.?.active = false;
    var strip_rows: u16 = 0;
    var opening: u8 = 0;
    var ceiling: u8 = 0;
    const e = cfgOk(cfg.?, &strip_rows, &opening, &ceiling);
    if (e != .ok) return e;
    viewAdopt(v.?, cfg.?, opening, ceiling, strip_rows);
    return .ok;
}

/// Swap the bound source under a live view (zoom_view_rebind): the geometry
/// re-clamps to the new extent and the input timer restarts.
pub fn viewRebind(v: ?*View, src: ?*const Source, now_ms: u32) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (src == null) {
        logErr("src must not be nullptr");
        return .null_ptr;
    }
    if (src.?.read == null) {
        logErr("source read seam must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("rebind on a closed view");
        return .invalid_state;
    }
    if (!dimOk(src.?.width) or !dimOk(src.?.height)) {
        logErr("source extent is zero or exceeds k_zoom_dim_max");
        return .invalid_arg;
    }
    v.?.src = src.?.*;
    reclamp(v.?);
    touch(v.?, now_ms);
    return .ok;
}

/// Mark the view owed a fresh flush (zoom_view_invalidate).
pub fn viewInvalidate(v: ?*View, now_ms: u32) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("invalidate on a closed view");
        return .invalid_state;
    }
    touch(v.?, now_ms);
    return .ok;
}

/// Pan direction (zoom_pan_t); none is the zero value and names no movement.
pub const Pan = enum(u8) { none = 0, left = 1, right = 2, up = 3, down = 4 };

/// (zoom_scale_cycle): zero-valued min lifts to the engine floor.
pub fn scaleCycle(scale: u8, min: u8, max: u8) u8 {
    const ladder_min: u8 = @intCast(scale_min);
    const floor_v: u8 = if (min == 0) ladder_min else min;
    const ceiling_v: u8 = if (max < floor_v) floor_v else max;
    if (scale >= ceiling_v) {
        return floor_v;
    }
    const doubled: u16 = @as(u16, scale) * @as(u16, 2);
    if (doubled >= ceiling_v) {
        return ceiling_v;
    }
    if (doubled < floor_v) {
        return floor_v;
    }
    return @intCast(doubled);
}

/// Change the magnification keeping the plane point under the focus fixed
/// (zoom_view_set_scale): the anchor moves by the rescale, then re-clamps.
pub fn viewSetScale(v: ?*View, scale: u8, focus_x: i32, focus_y: i32, now_ms: u32) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("set_scale on a closed view");
        return .invalid_state;
    }
    if (scale < @as(u8, @intCast(scale_min)) or scale > v.?.scale_max) {
        logErr("requested scale is off the ladder");
        return .out_of_range;
    }
    if (scale == v.?.scale) {
        return .ok;
    }
    const off_x: i32 = focus_x - v.?.dst.x;
    const off_y: i32 = focus_y - v.?.dst.y;
    const old: i64 = v.?.scale;
    const plane_x: i64 = @as(i64, v.?.anchor_x + off_x) * @as(i64, scale);
    const plane_y: i64 = @as(i64, v.?.anchor_y + off_y) * @as(i64, scale);
    const scaled_x: i64 = @divTrunc(plane_x, old);
    const scaled_y: i64 = @divTrunc(plane_y, old);
    v.?.anchor_x = @intCast(scaled_x - off_x);
    v.?.anchor_y = @intCast(scaled_y - off_y);
    v.?.scale = scale;
    reclamp(v.?);
    touch(v.?, now_ms);
    return .ok;
}

/// Move the viewport by a caller delta (zoom_view_pan): a pan that clamps to
/// no movement owes the panel nothing.
pub fn viewPan(v: ?*View, dx: i32, dy: i32, now_ms: u32) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("pan on a closed view");
        return .invalid_state;
    }
    const was_x: i32 = v.?.anchor_x;
    const was_y: i32 = v.?.anchor_y;
    v.?.anchor_x = satAdd(v.?.anchor_x, dx);
    v.?.anchor_y = satAdd(v.?.anchor_y, dy);
    reclamp(v.?);
    if ((v.?.anchor_x != was_x) or (v.?.anchor_y != was_y)) {
        touch(v.?, now_ms);
    }
    return .ok;
}

/// Pan one viewport step toward a direction (zoom_view_pan_dir): each step
/// advances by the fraction of the viewport that is NOT retained as overlap.
pub fn viewPanDir(v: ?*View, dir: Pan, now_ms: u32) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("pan_dir on a closed view");
        return .invalid_state;
    }
    const step_x: i32 = v.?.dst.w - @divTrunc(v.?.dst.w, overlap_div);
    const step_y: i32 = v.?.dst.h - @divTrunc(v.?.dst.h, overlap_div);
    var dx: i32 = 0;
    var dy: i32 = 0;
    switch (dir) {
        .left => dx = -step_x,
        .right => dx = step_x,
        .up => dy = -step_y,
        .down => dy = step_y,
        .none => {},
    }
    return viewPan(v, dx, dy, now_ms);
}

/// The refresh decision a panel consumes (zoom_present_t).
pub const Present = extern struct {
    rect: Rect = .{},
    refresh: Refresh = .fast,
    present: bool = false,
};

/// Source-space window the viewport samples (zoom_view_window).
pub fn viewWindow(v: ?*const View, out: ?*Rect) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        logErr("out rect must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("window query on a closed view");
        return .invalid_state;
    }
    const ax = axis(v.?.anchor_x, v.?.dst.w, @intCast(v.?.scale), @intCast(v.?.src.width));
    const ay = axis(v.?.anchor_y, v.?.dst.h, @intCast(v.?.scale), @intCast(v.?.src.height));
    out.?.x = @intCast(ax.s0);
    out.?.w = @intCast(ax.count);
    out.?.y = @intCast(ay.s0);
    out.?.h = @intCast(ay.count);
    return .ok;
}

/// Settle tick (zoom_view_tick): only an open view with an armed settle
/// promotes to quality; the millisecond difference is unsigned, so a
/// wrapping counter settles early rather than late.
pub fn viewTick(v: ?*View, now_ms: u32) bool {
    if (v == null) {
        return false;
    }
    if (!v.?.active or !v.?.settle_armed) {
        return false;
    }
    if (now_ms -% v.?.last_input_ms < v.?.settle_ms) {
        return false;
    }
    v.?.settle_armed = false;
    v.?.pending = true;
    v.?.pending_kind = .quality;
    return true;
}

/// Hand the refresh decision to the panel and clear the pending flag
/// (zoom_view_present).
pub fn viewPresent(v: ?*View, out: ?*Present) Err {
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        logErr("present out must not be nullptr");
        return .null_ptr;
    }
    if (!v.?.active) {
        logErr("present on a closed view");
        return .invalid_state;
    }
    out.?.rect = v.?.dst;
    out.?.refresh = v.?.pending_kind;
    out.?.present = v.?.pending;
    v.?.pending = false;
    return .ok;
}
