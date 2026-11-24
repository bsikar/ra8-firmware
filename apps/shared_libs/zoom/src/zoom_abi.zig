//! C ABI surface of the zoom facade (`apps/shared_libs/zoom`).
//!
//! `inc/zoom.h` and `src/zoom_internal.h` are unchanged: this file exports
//! the same public `zoom_view_*` / `zoom_scale_cycle` symbols and the module
//! private `priv_zoom_axis` that `zoom.c` did, with the same guard order, the
//! same codes and the same log lines. Every decision lives in `zoom.zig`;
//! what stays here is pointer nullability and the writes into caller-owned
//! storage. The layout fixture in zoom.zig pins the struct parity.
//!
//! External symbols: `ra8_log_emit_error` (ra8_log.h's backend entry), which
//! ra8_core_hal's own objects supply at the final link.

const std = @import("std");
pub const zoom = @import("zoom.zig");
pub const book = @import("zoom_book.zig");
pub const mem = @import("zoom_mem.zig");
pub const popup = @import("zoom_popup.zig");
pub const render = @import("zoom_render.zig");
pub const tiles = @import("zoom_tiles.zig");

const Err = zoom.Err;
const View = zoom.View;
const Source = zoom.Source;
const Cfg = zoom.Cfg;
const Rect = zoom.Rect;
const Present = zoom.Present;
const Axis = zoom.Axis;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;

// ---------------------------------------------------------------------------
// The public zoom.h surface.
// ---------------------------------------------------------------------------

export fn zoom_view_open(v: ?*View, cfg: ?*const Cfg) callconv(.c) Err {
    return zoom.viewOpen(v, cfg);
}

export fn zoom_view_rebind(v: ?*View, src: ?*const Source, now_ms: u32) callconv(.c) Err {
    return zoom.viewRebind(v, src, now_ms);
}

export fn zoom_view_invalidate(v: ?*View, now_ms: u32) callconv(.c) Err {
    return zoom.viewInvalidate(v, now_ms);
}

export fn zoom_view_close(v: ?*View) callconv(.c) Err {
    if (v == null) {
        zoom.logErr("view must not be nullptr");
        return .null_ptr;
    }
    zoom.close(v.?);
    return .ok;
}

export fn zoom_view_active(v: ?*const View) callconv(.c) bool {
    return (v != null) and v.?.active;
}

export fn zoom_scale_cycle(scale: u8, min: u8, max: u8) callconv(.c) u8 {
    return zoom.scaleCycle(scale, min, max);
}

export fn zoom_view_set_scale(v: ?*View, scale: u8, focus_x: i32, focus_y: i32, now_ms: u32) callconv(.c) Err {
    return zoom.viewSetScale(v, scale, focus_x, focus_y, now_ms);
}

export fn zoom_view_pan(v: ?*View, dx: i32, dy: i32, now_ms: u32) callconv(.c) Err {
    return zoom.viewPan(v, dx, dy, now_ms);
}

export fn zoom_view_pan_dir(v: ?*View, dir: zoom.Pan, now_ms: u32) callconv(.c) Err {
    return zoom.viewPanDir(v, dir, now_ms);
}

export fn zoom_view_window(v: ?*const View, out: ?*Rect) callconv(.c) Err {
    return zoom.viewWindow(v, out);
}

export fn zoom_view_tick(v: ?*View, now_ms: u32) callconv(.c) bool {
    return zoom.viewTick(v, now_ms);
}

export fn zoom_view_present(v: ?*View, out: ?*Present) callconv(.c) Err {
    return zoom.viewPresent(v, out);
}

// The strip composite (zoom_render.c's zoom_view_render): the render half of
// the facade. zoom_render.zig owns every decision; the export only carries
// the C ABI.
export fn zoom_view_render(v: ?*View) callconv(.c) Err {
    return render.viewRender(v);
}

// ---------------------------------------------------------------------------
// The module-private geometry helper zoom_render.zig (RA8FW-1260) and the host zoom suite
// call (zoom_internal.h). zoom.zig's axis() is the single definition; the
// export just carries the C-side zoom_axis_t write.
// ---------------------------------------------------------------------------

export fn priv_zoom_axis(anchor: i32, extent: i32, scale: i32, src_dim: i32, out: ?*Axis) callconv(.c) void {
    out.?.* = zoom.axis(anchor, extent, scale, src_dim);
}

// ---------------------------------------------------------------------------
// The source binder zoom_tiles.c and zoom_book.c call (zoom.h's
// zoom_source_init). zoom.zig's sourceInit owns the guards, the dimension cap
// and the writes into caller storage; the export only carries the C ABI,
// zoom_read_fn optionality included.
// ---------------------------------------------------------------------------

export fn zoom_source_init(
    out: ?*Source,
    read: zoom.ReadFn,
    ctx: ?*anyopaque,
    width: u32,
    height: u32,
) callconv(.c) Err {
    return zoom.sourceInit(out, read, ctx, width, height);
}

// ---------------------------------------------------------------------------
// The tiled-atlas adapter (zoom_tiles.c's four functions, RA8FW-1261). The
// membrane owns pointer nullability (the C module's RA8_CHECK_NULL_PTR on
// the read destination), the writes into caller-owned storage, and the
// zoom_pan_t -> ra8_tile_pan_dir_t forwarding whose value agreement
// zoom_tiles.zig pins at compile time.
// ---------------------------------------------------------------------------

export fn zoom_tile_src_init(ts: ?*tiles.TileSrc, cache: ?*tiles.TileCache, image_id: u32, width: u32, height: u32, tile_w: u16, tile_h: u16) callconv(.c) Err {
    return tiles.tileSrcInit(ts, cache, image_id, width, height, tile_w, tile_h);
}

export fn zoom_tile_src_bind(ts: ?*tiles.TileSrc, out: ?*Source) callconv(.c) Err {
    return tiles.tileSrcBind(ts, out);
}

export fn zoom_tile_read(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
    if (out == null) {
        zoom.logErr("read destination must not be nullptr");
        return .null_ptr;
    }
    return tiles.tileRead(ctx, x, y, w, h, out.?, out_stride);
}

export fn zoom_tiles_prefetch(ts: ?*tiles.TileSrc, v: ?*const View, dir: zoom.Pan, max_tiles: u16, out_warmed: ?*u16) callconv(.c) Err {
    return tiles.tilesPrefetch(ts, v, dir, max_tiles, out_warmed);
}

// ---------------------------------------------------------------------------
// In-memory gray8 figure adapter (zoom_mem.h).
// ---------------------------------------------------------------------------

export fn zoom_mem_src_init(out: ?*mem.MemSrc, pixels: ?[*]const u8, width: u32, height: u32, stride: u32) callconv(.c) Err {
    return mem.memSrcInit(out, pixels, width, height, stride);
}

export fn zoom_mem_src_bind(src: ?*const mem.MemSrc, out: ?*Source) callconv(.c) Err {
    return mem.memSrcBind(src, out);
}

export fn zoom_mem_read(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
    return mem.memReadImpl(ctx, x, y, w, h, out, out_stride);
}

// ---------------------------------------------------------------------------
// EPUB figure-zoom popup controller (zoom_popup.h).
// ---------------------------------------------------------------------------

export fn zoom_popup_open(p: ?*popup.Popup, cfg: ?*const popup.PopupCfg) callconv(.c) Err {
    return popup.popupOpen(p, cfg);
}

export fn zoom_popup_close(p: ?*popup.Popup) callconv(.c) Err {
    return popup.popupClose(p);
}

export fn zoom_popup_active(p: ?*const popup.Popup) callconv(.c) bool {
    return (p != null) and p.?.active;
}

export fn zoom_popup_zone_hit(p: ?*const popup.Popup, x: i32, y: i32) callconv(.c) popup.PopupZone {
    return popup.popupZoneHit(p, x, y);
}

export fn zoom_popup_tap(p: ?*popup.Popup, x: i32, y: i32, now_ms: u32, out_action: ?*popup.PopupAction) callconv(.c) Err {
    return popup.popupTap(p, x, y, now_ms, out_action);
}

export fn zoom_popup_tick(p: ?*popup.Popup, now_ms: u32) callconv(.c) bool {
    return popup.popupTick(p, now_ms);
}

export fn zoom_popup_present(p: ?*popup.Popup, out: ?*Present) callconv(.c) Err {
    return popup.popupPresent(p, out);
}

export fn zoom_popup_render(p: ?*popup.Popup) callconv(.c) Err {
    return popup.popupRender(p);
}

export fn zoom_popup_status(p: ?*const popup.Popup, buf: ?[*]u8, buf_cap: usize) callconv(.c) Err {
    return popup.popupStatus(p, buf, buf_cap);
}

// ---------------------------------------------------------------------------
// The ABI membrane's own probe suite: every export driven over the C-shaped
// structs, including the null guards zoom.c had and the log path.
// ---------------------------------------------------------------------------

fn nullRead(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    _ = ctx;
    _ = x;
    _ = y;
    _ = w;
    _ = h;
    _ = out;
    _ = out_stride;
    return .ok;
}

test "abi exports carry the C guards" {
    var v = View{};
    try std.testing.expectEqual(Err.null_ptr, zoom_view_open(null, null));
    var cfg = Cfg{ .src = .{ .read = nullRead, .width = 16, .height = 16 }, .dst = .{ .w = 8, .h = 8 } };
    var scratch: [16]u8 = undefined;
    cfg.scratch = .{ .row = scratch[0..].ptr, .strip = scratch[0..].ptr, .packed_buf = scratch[0..].ptr, .row_cap = 8, .strip_cap = 8, .packed_cap = 4 };
    try std.testing.expectEqual(Err.ok, zoom_view_open(&v, &cfg));
    try std.testing.expect(zoom_view_active(&v));
    try std.testing.expectEqual(Err.ok, zoom_view_close(&v));
    try std.testing.expect(!zoom_view_active(&v));
    // A null view is a null_ptr rejection on every fallible entry point.
    try std.testing.expectEqual(Err.null_ptr, zoom_view_rebind(null, null, 0));
    try std.testing.expectEqual(Err.null_ptr, zoom_view_invalidate(null, 0));
    try std.testing.expectEqual(Err.null_ptr, zoom_view_set_scale(null, 1, 0, 0, 0));
    try std.testing.expectEqual(Err.null_ptr, zoom_view_pan(null, 0, 0, 0));
    try std.testing.expectEqual(Err.null_ptr, zoom_view_pan_dir(null, .none, 0));
    var win = Rect{};
    try std.testing.expectEqual(Err.null_ptr, zoom_view_window(null, &win));
    try std.testing.expect(!zoom_view_tick(null, 0));
    var pres = Present{};
    try std.testing.expectEqual(Err.null_ptr, zoom_view_present(null, &pres));
}

test "priv_zoom_axis resolves one axis like the C did" {
    var ax = Axis{};
    // x4 magnification of a 16px source, 32px of viewport: the destination
    // window is the full viewport and the source span is (31/4 - 0/4) + 1.
    priv_zoom_axis(0, 32, 4, 16, &ax);
    try std.testing.expectEqual(@as(i32, 0), ax.d0);
    try std.testing.expectEqual(@as(i32, 32), ax.d1);
    try std.testing.expectEqual(@as(u32, 0), ax.s0);
    try std.testing.expectEqual(@as(u32, 8), ax.count);
    // An anchor past the magnified plane: the window collapses, count is 0.
    priv_zoom_axis(100, 32, 2, 8, &ax);
    try std.testing.expectEqual(@as(i32, 0), ax.d0);
    try std.testing.expectEqual(@as(i32, 0), ax.d1);
    try std.testing.expectEqual(@as(u32, 0), ax.count);
}

test "zoom_mem and zoom_popup C ABI exports carry C guards and state" {
    const px = [_]u8{ 0x10, 0x20, 0x30, 0x40 };
    var ms = mem.MemSrc{};
    try std.testing.expectEqual(Err.ok, zoom_mem_src_init(&ms, &px, 2, 2, 0));
    var src = Source{};
    try std.testing.expectEqual(Err.ok, zoom_mem_src_bind(&ms, &src));
    var out: [4]u8 = undefined;
    try std.testing.expectEqual(Err.ok, zoom_mem_read(&ms, 0, 0, 2, 2, &out, 2));
    try std.testing.expectEqual([4]u8{ 0x10, 0x20, 0x30, 0x40 }, out);

    var p = popup.Popup{};
    try std.testing.expect(!zoom_popup_active(&p));
    try std.testing.expect(!zoom_popup_tick(&p, 0));
    try std.testing.expectEqual(popup.PopupZone.none, zoom_popup_zone_hit(&p, 0, 0));
    var scratch: [128]u8 = undefined;
    const cfg = popup.PopupCfg{
        .src = src,
        .scratch = .{ .row = &scratch, .row_cap = 64, .strip = &scratch, .strip_cap = 128, .packed_buf = &scratch, .packed_cap = 64 },
        .fb_w = 64,
        .fb_h = 64,
    };
    try std.testing.expectEqual(Err.ok, zoom_popup_open(&p, &cfg));
    try std.testing.expect(zoom_popup_active(&p));
    var act: popup.PopupAction = .none;
    try std.testing.expectEqual(Err.ok, zoom_popup_tap(&p, 2, 2, 10, &act));
    try std.testing.expectEqual(popup.PopupAction.closed, act);
    try std.testing.expect(!zoom_popup_active(&p));
}
