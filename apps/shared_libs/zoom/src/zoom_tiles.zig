// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Zig port of the tiled-atlas source adapter (`inc/zoom_tiles.h`,
//! RA8FW-1261): pixel rect -> covering tiles -> per-tile acquire, copy the
//! overlap, release.
//!
//! The acquire/release pair brackets exactly one copy, so at most one cell
//! is ever pinned and the resident set is whatever the cache holds rather
//! than whatever the rectangle spans, which is what makes "only visible
//! tiles are resident" a property of the code rather than a hope.
//!
//! The C TU compiled to nothing when `ra8_tile_cache.h` was not reachable,
//! so an app that magnifies only `.rabook` figures never linked `ra8_mem`.
//! The Zig archive keeps that property structurally, exactly like
//! `zoom_book.zig`: `ra8_tile_cache_*` / `ra8_tile_rect_of_pixels` are
//! referenced only from the `zoom_tile_*` exports, so a consumer that never
//! calls them never pulls this object out of the archive.
//!
//! External symbols: `ra8_tile_cache_get`, `ra8_tile_cache_put`,
//! `ra8_tile_cache_prefetch_pan` and `ra8_tile_rect_of_pixels`
//! (ra8_tile_cache.h), supplied by `ra8_mem` at the final link, and
//! `ra8_log_emit_error` (ra8_log.h's backend entry), supplied by
//! ra8_core_hal. The standalone test build installs silent/stub versions of
//! all of them; the firmware build links the real definitions.
//!
const std = @import("std");

pub const zoom = @import("zoom.zig");

const Err = zoom.Err;
const Source = zoom.Source;
const View = zoom.View;
const Rect = zoom.Rect;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;
extern fn ra8_tile_cache_get(tc: ?*TileCache, key: ?*const TileKey, out_tile: ?*Tile) callconv(.c) Err;
extern fn ra8_tile_cache_put(tc: ?*TileCache, pixels: ?[*]const u8) callconv(.c) Err;
extern fn ra8_tile_cache_prefetch_pan(tc: ?*TileCache, req: ?*const TilePrefetchReq, out_warmed: ?*u16) callconv(.c) Err;
extern fn ra8_tile_rect_of_pixels(px: u32, py: u32, pw: u32, ph: u32, tile_w: u16, tile_h: u16, tile_cols: u16, tile_rows: u16, out: ?*TileRect) callconv(.c) Err;

/// Mirror of ra8_tile_key_t (libs/ra8_mem/inc/ra8_tile_cache.h). The key is
/// compared byte-wise over its full extent, so its layout parity is pinned:
/// 4 + 2 + 2 + 2 + 2 bytes, padding-free.
pub const TileKey = extern struct {
    image_id: u32,
    tile_x: u16,
    tile_y: u16,
    zoom: u16,
    reserved: u16,
};

/// Mirror of ra8_tile_t: a pinned view of a cached tile returned by
/// ra8_tile_cache_get and released back through ra8_tile_cache_put.
pub const Tile = extern struct {
    pixels: ?[*]const u8 = null,
    width: u16 = 0,
    height: u16 = 0,
};

/// Mirror of ra8_tile_rect_t: an inclusive tile rectangle (ra8_mem's
/// `tx0..tx1` / `ty0..ty1` naming kept verbatim).
pub const TileRect = extern struct {
    tx0: u16 = 0,
    ty0: u16 = 0,
    tx1: u16 = 0,
    ty1: u16 = 0,
};

/// Mirror of ra8_tile_prefetch_req_t: what ra8_tile_cache_prefetch_pan
/// warms around the viewport. Field order matches the C struct.
pub const TilePrefetchReq = extern struct {
    image_id: u32 = 0,
    view: TileRect = .{},
    tile_cols: u16 = 0,
    tile_rows: u16 = 0,
    zoom: u16 = 0,
    max_tiles: u16 = 0,
    dir: u8 = 0,
};

/// Opaque to this adapter: the cache type is owned by ra8_mem and only ever
/// passed through (ra8_tile_cache_t).
pub const TileCache = opaque {};

/// A tiled gray8 image, its grid, and the cache that pages it
/// (zoom_tile_src_t). Caller-owned storage; zero it and call tileSrcInit.
pub const TileSrc = extern struct {
    cache: ?*TileCache = null,
    image_id: u32 = 0,
    width: u32 = 0,
    height: u32 = 0,
    tile_w: u16 = 0,
    tile_h: u16 = 0,
    tile_cols: u16 = 0,
    tile_rows: u16 = 0,
};

/// The requested pixel rectangle, carried through the per-tile copy, so the
/// copy helper takes one argument instead of four (zoom_tile_span_t).
const Span = struct {
    x: u32,
    y: u32,
    w: u32,
    h: u32,
};

/// Tile-cache key zoom level: native pixels. This viewer magnifies the
/// native pixels rather than paging a pre-scaled pyramid, so every key it
/// forms names the native level (k_zoom_tiles_native_level == 0, the tile
/// cache's native level).
pub const native_level: u16 = 0;

/// Component tag for this module's log lines (zoom_tiles.c s_tag).
const s_tag: [*:0]const u8 = "zoom_tiles";

fn emitLog(msg: [*:0]const u8) void {
    if (zoom.log_hook) |f| {
        f(std.mem.span(msg));
    } else {
        ra8_log_emit_error(s_tag, msg);
    }
}

/// One log line per failure the C module emits through RA8_CHECK_* /
/// RA8_RETURN_ON_ERROR (zoom_tiles.c), routed through the C core's
/// ra8_log_emit_error under the module's tag unless a hook overrides.
fn logErr(comptime msg: [:0]const u8) void {
    emitLog(msg);
}

/// RA8_RETURN_ON_ERROR: the macro logs the step name and returns the code
/// verbatim; so does this.
fn propagate(e: Err, comptime msg: [:0]const u8) Err {
    if (e != .ok) logErr(msg);
    return e;
}

comptime {
    if (@sizeOf(TileKey) != 4 + 2 + 2 + 2 + 2) {
        @compileError("TileKey layout parity with ra8_tile_key_t broken (12 B, padding-free)");
    }
    if (@sizeOf(TileRect) != 2 * 4) {
        @compileError("TileRect layout parity with ra8_tile_rect_t broken (8 B)");
    }
    // zoom_tiles_prefetch forwards a zoom_pan_t straight into the cache's
    // ra8_tile_pan_dir_t rather than switching over it. That is only sound
    // while the two enumerations agree value for value, so the agreement is
    // pinned here rather than asserted in prose: a renumbering of either
    // enum fails the build instead of quietly warming the wrong edge.
    if (@backingInt(zoom.Pan.none) != 0) {
        @compileError("zoom_pan_t and ra8_tile_pan_dir_t must agree: none");
    }
    if (@backingInt(zoom.Pan.left) != 1) {
        @compileError("zoom_pan_t and ra8_tile_pan_dir_t must agree: left");
    }
    if (@backingInt(zoom.Pan.right) != 2) {
        @compileError("zoom_pan_t and ra8_tile_pan_dir_t must agree: right");
    }
    if (@backingInt(zoom.Pan.up) != 3) {
        @compileError("zoom_pan_t and ra8_tile_pan_dir_t must agree: up");
    }
    if (@backingInt(zoom.Pan.down) != 4) {
        @compileError("zoom_pan_t and ra8_tile_pan_dir_t must agree: down");
    }
    if (native_level != 0) {
        @compileError("k_zoom_tiles_native_level must stay 0, the tile cache's native level");
    }
}

/// Copy one tile's overlap with the requested rectangle into the output
/// (internal_tile_copy). Both rectangles are half-open, so the overlap is
/// the usual [max(starts), min(ends)) on each axis; an empty overlap copies
/// nothing (the caller only ever passes intersecting tiles, so it is a
/// guard, not a path). Row addresses are computed once per row from the
/// tile origin and the output stride.
fn tileCopy(tile: *const Tile, org_x: u32, org_y: u32, span: Span, out: [*]u8, stride: u32) void {
    const x0 = @max(span.x, org_x);
    const y0 = @max(span.y, org_y);
    const tx_end = org_x + @as(u32, tile.width);
    const ty_end = org_y + @as(u32, tile.height);
    const x1 = @min(span.x + span.w, tx_end);
    const y1 = @min(span.y + span.h, ty_end);
    var sy = y0;
    while (sy < y1) : (sy += 1) {
        const src_row: [*]const u8 = tile.pixels.? +
            @as(usize, sy - org_y) * @as(usize, tile.width) + (x0 - org_x);
        const dst_row: [*]u8 = out + @as(usize, sy - span.y) * stride + (x0 - span.x);
        @memcpy(dst_row[0..(x1 - x0)], src_row[0..(x1 - x0)]);
    }
}

/// Fetch one tile, copy its overlap, and release it (internal_tile_fetch).
/// The fetched tile's decoded extent is cross-checked against the geometry
/// declared at bind time before a single byte is read: a decoder that
/// produced RGB tiles, or a different tile edge, shows up here as a size
/// mismatch rather than as a wrong picture. The cell is released on every
/// return path.
fn tileFetch(ts: *TileSrc, tx: u16, ty: u16, span: Span, out: [*]u8, stride: u32) Err {
    const key = TileKey{
        .image_id = ts.image_id,
        .tile_x = tx,
        .tile_y = ty,
        .zoom = native_level,
        .reserved = 0,
    };
    var tile = Tile{};
    const got = ra8_tile_cache_get(ts.cache, &key, &tile);
    if (got != .ok) {
        logErr("tile fetch");
        return got;
    }

    const org_x = @as(u32, tx) * ts.tile_w;
    const org_y = @as(u32, ty) * ts.tile_h;
    const exp_w = if (org_x + ts.tile_w > ts.width) ts.width - org_x else ts.tile_w;
    const exp_h = if (org_y + ts.tile_h > ts.height) ts.height - org_y else ts.tile_h;
    // Decision: a tile that did not decode to the declared gray8 geometry is
    // a format mismatch, not a picture (2 conditions).
    if (@as(u32, tile.width) != exp_w or @as(u32, tile.height) != exp_h) {
        _ = ra8_tile_cache_put(ts.cache, tile.pixels);
        logErr("tile decoded to an unexpected extent (wrong bpp or geometry)");
        return .invalid_size;
    }
    tileCopy(&tile, org_x, org_y, span, out, stride);
    return ra8_tile_cache_put(ts.cache, tile.pixels);
}

/// Fetch, copy and release one inclusive run of tiles along a tile row
/// (internal_walk_tile_row). Split out of walkTiles purely so the tile walk
/// stays inside the project's nesting bar; behaviourally it is the inner
/// loop, unchanged.
fn walkTileRow(ts: *TileSrc, ty: u16, tx0: u16, tx1: u16, span: Span, out: [*]u8, stride: u32) Err {
    var tx = tx0;
    while (tx <= tx1) : (tx += 1) {
        const e = tileFetch(ts, tx, ty, span, out, stride);
        if (e != .ok) {
            logErr("tile copy");
            return e;
        }
    }
    return .ok;
}

/// Fetch, copy and release every tile of an inclusive tile rectangle
/// (internal_walk_tiles). The residency claim in one loop: tiles are visited
/// one at a time and each is released before the next is acquired, so the
/// pin count never exceeds one and a cache smaller than the rectangle
/// degrades to re-decoding rather than to a no_mem error.
fn walkTiles(ts: *TileSrc, tiles: *const TileRect, span: Span, out: [*]u8, stride: u32) Err {
    var ty = tiles.ty0;
    while (ty <= tiles.ty1) : (ty += 1) {
        const e = walkTileRow(ts, ty, tiles.tx0, tiles.tx1, span, out, stride);
        if (e != .ok) {
            logErr("tile row");
            return e;
        }
    }
    return .ok;
}

/// Validate a requested read rectangle against the bound image
/// (internal_read_rect_ok). Two fail-closed rules, kept out of tileRead so
/// the read body is the tile walk and nothing else. Neither is a clamp: the
/// zoom engine only ever asks for rectangles inside the extent it was handed
/// at bind time, so anything else is a caller defect, and silently shrinking
/// it would hide the real bug behind a partly-correct image.
fn readRectOk(ts: *const TileSrc, x: u32, y: u32, w: u32, h: u32, out_stride: u32) Err {
    // Decision: an empty rectangle, or one narrower than its own stride, is
    // a caller defect rather than something to clamp (3 conditions).
    if (w == 0 or h == 0 or out_stride < w) {
        logErr("read rect must be non-empty and fit its stride");
        return .invalid_arg;
    }
    // Decision: the rectangle must lie wholly inside the bound image.
    if (x + w > ts.width or y + h > ts.height) {
        logErr("read rect leaves the bound image");
        return .out_of_range;
    }
    return .ok;
}

/// Bind a tiled image and its cache, deriving the tile grid
/// (zoom_tile_src_init). Derives tile_cols / tile_rows rather than accepting
/// them, so the grid can never disagree with the extent it is supposed to
/// cover.
pub fn tileSrcInit(ts: ?*TileSrc, cache: ?*TileCache, image_id: u32, width: u32, height: u32, tile_w: u16, tile_h: u16) Err {
    if (ts == null) {
        logErr("binding must not be nullptr");
        return .null_ptr;
    }
    if (cache == null) {
        logErr("tile cache must not be nullptr");
        return .null_ptr;
    }
    // Decision: every extent and tile edge must be non-zero (4 conditions).
    if (width == 0 or height == 0 or tile_w == 0 or tile_h == 0) {
        logErr("image extent and tile edges must all be non-zero");
        return .invalid_arg;
    }
    const cols = (width + tile_w - 1) / tile_w;
    const rows = (height + tile_h - 1) / tile_h;
    // Decision: the derived grid must fit the cache key's 16-bit tile
    // indices.
    if (cols > std.math.maxInt(u16) or rows > std.math.maxInt(u16)) {
        logErr("derived tile grid exceeds the 16-bit tile index range");
        return .invalid_size;
    }
    ts.?.cache = cache;
    ts.?.image_id = image_id;
    ts.?.width = width;
    ts.?.height = height;
    ts.?.tile_w = tile_w;
    ts.?.tile_h = tile_h;
    ts.?.tile_cols = @intCast(cols);
    ts.?.tile_rows = @intCast(rows);
    return .ok;
}

/// Bind the adapter as a zoom source (zoom_tile_src_bind): the engine's
/// read seam points at tileRead with the binding as its context, and the
/// extent is the bound image's own.
pub fn tileSrcBind(ts: ?*TileSrc, out: ?*Source) Err {
    if (ts == null) {
        logErr("binding must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        logErr("source out must not be nullptr");
        return .null_ptr;
    }
    if (ts.?.cache == null) {
        logErr("binding was never initialised");
        return .null_ptr;
    }
    return zoom.sourceInit(out, tileRead, ts.?, ts.?.width, ts.?.height);
}

/// The gray8 sub-rectangle reader (zoom_tile_read): validate the rectangle,
/// map it onto tiles, and walk them. ctx is the TileSrc the binder handed
/// the engine.
pub fn tileRead(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    if (ctx == null) {
        logErr("read ctx must not be nullptr");
        return .null_ptr;
    }
    const ts: *TileSrc = @ptrCast(@alignCast(ctx.?));
    if (ts.cache == null) {
        logErr("binding was never initialised");
        return .null_ptr;
    }
    const rect = readRectOk(ts, x, y, w, h, out_stride);
    if (rect != .ok) {
        logErr("read rect check");
        return rect;
    }

    var tiles = TileRect{};
    const mapped = ra8_tile_rect_of_pixels(x, y, w, h, ts.tile_w, ts.tile_h, ts.tile_cols, ts.tile_rows, &tiles);
    if (mapped != .ok) {
        logErr("pixel rect to tile rect");
        return mapped;
    }

    const span = Span{ .x = x, .y = y, .w = w, .h = h };
    return walkTiles(ts, &tiles, span, out, out_stride);
}

/// Warm the tiles one step of viewport travel will reveal
/// (zoom_tiles_prefetch): the visible window mapped onto tiles, handed to
/// the cache's directional prefetcher with the pan direction forwarded
/// verbatim (value agreement pinned at compile time above).
pub fn tilesPrefetch(ts: ?*TileSrc, v: ?*const View, dir: zoom.Pan, max_tiles: u16, out_warmed: ?*u16) Err {
    if (ts == null) {
        logErr("binding must not be nullptr");
        return .null_ptr;
    }
    if (v == null) {
        logErr("view must not be nullptr");
        return .null_ptr;
    }
    if (ts.?.cache == null) {
        logErr("binding was never initialised");
        return .null_ptr;
    }

    var win = Rect{};
    const windowed = zoom.viewWindow(v, &win);
    if (windowed != .ok) {
        logErr("visible window");
        return windowed;
    }

    var tiles = TileRect{};
    const mapped = ra8_tile_rect_of_pixels(
        @intCast(win.x),
        @intCast(win.y),
        @intCast(win.w),
        @intCast(win.h),
        ts.?.tile_w,
        ts.?.tile_h,
        ts.?.tile_cols,
        ts.?.tile_rows,
        &tiles,
    );
    if (mapped != .ok) {
        logErr("visible window to tile rect");
        return mapped;
    }

    const req = TilePrefetchReq{
        .image_id = ts.?.image_id,
        .view = tiles,
        .tile_cols = ts.?.tile_cols,
        .tile_rows = ts.?.tile_rows,
        .zoom = native_level,
        .max_tiles = max_tiles,
        .dir = @backingInt(dir),
    };
    return ra8_tile_cache_prefetch_pan(ts.?.cache, &req, out_warmed);
}
