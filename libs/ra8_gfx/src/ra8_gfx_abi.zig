//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the `ra8_gfx` software rasteriser core. Defines the one
//! module-wide framebuffer binding `g_gfx_text_state`, exports the two
//! promoted helpers `priv_gfx_text_pack_565` and `priv_gfx_text_plot` that the
//! remaining C translation units (dither, text/glyph, font table) reach
//! through `src/ra8_gfx_internal.h`, and exports the eleven public entry points
//! declared in `inc/ra8_gfx.h`, including the packed-gray4 loupe zoom blit.
//!
//! Every decision lives in `internal/root.zig`; this file only moves bytes.

const std = @import("std");
const impl = @import("internal/root.zig");

/// Re-exported so the ABI test binary shares the exact struct types.
pub const internal = impl;

/// `g_gfx_text_state` -- the single shared framebuffer binding. The C
/// definition initialised only `.format`, so RGB565 is the pre-init format.
pub export var g_gfx_text_state: impl.State = .{ .format = impl.format.rgb565 };

/// The active clip box as a value.
fn clip() impl.Box {
    return .{
        .x0 = g_gfx_text_state.clip_x0,
        .y0 = g_gfx_text_state.clip_y0,
        .x1 = g_gfx_text_state.clip_x1,
        .y1 = g_gfx_text_state.clip_y1,
    };
}

/// Row stride of the bound framebuffer, in bytes.
fn stride() usize {
    return @as(usize, g_gfx_text_state.width) * @as(usize, g_gfx_text_state.bpp);
}

/// `priv_gfx_text_pack_565`
pub export fn priv_gfx_text_pack_565(color: u32) callconv(.c) u16 {
    return impl.pack565(color);
}

/// `priv_gfx_text_plot`
pub export fn priv_gfx_text_plot(x: i32, y: i32, color: u32) callconv(.c) void {
    if (!impl.plotInClip(x, y, clip())) return;
    const fb = g_gfx_text_state.fb orelse return;
    impl.putPixel(
        fb,
        stride(),
        g_gfx_text_state.format,
        @intCast(x),
        @intCast(y),
        color,
    );
}

/// One row of an RGB565 span: a `memset` when the two halves match, an
/// interleaving store otherwise. Mirrors `internal_fill_565`.
fn fill565(p: [*]u8, count: usize, lo: u8, hi: u8) void {
    if (impl.fillIsFlat(lo, hi)) {
        @memset(p[0 .. count * impl.rgb565_bpp], lo);
        return;
    }
    var i: usize = 0;
    while (i < count) : (i += 1) {
        p[(i * impl.rgb565_bpp)] = lo;
        p[(i * impl.rgb565_bpp) + 1] = hi;
    }
}

/// `internal_fill_rect_565`
fn fillRect565(x: i32, y: i32, w: i32, h: i32, color: u32) void {
    const box = impl.fillSpan(x, y, w, h, clip()) orelse return;
    const fb = g_gfx_text_state.fb orelse return;

    const v = impl.pack565(color);
    const lo: u8 = @intCast(v & 0xFF);
    const hi: u8 = @intCast((v >> 8) & 0xFF);
    const bpp: usize = g_gfx_text_state.bpp;
    const row_bytes = stride();
    const count: usize = @intCast(box.x1 - box.x0);

    var row = box.y0;
    while (row < box.y1) : (row += 1) {
        const offset = (@as(usize, @intCast(row)) * row_bytes) + (@as(usize, @intCast(box.x0)) * bpp);
        fill565(fb + offset, count, lo, hi);
    }
}

/// `internal_fill_rect`
fn fillRect(x: i32, y: i32, w: i32, h: i32, color: u32) void {
    if (g_gfx_text_state.format == impl.format.rgb565) {
        fillRect565(x, y, w, h, color);
        return;
    }
    var row: i32 = 0;
    while (row < h) : (row += 1) {
        var col: i32 = 0;
        while (col < w) : (col += 1) {
            priv_gfx_text_plot(x +% col, y +% row, color);
        }
    }
}

/// `internal_rect_outline`
fn rectOutline(x: i32, y: i32, w: i32, h: i32, color: u32) void {
    var col: i32 = 0;
    while (col < w) : (col += 1) {
        priv_gfx_text_plot(x +% col, y, color);
        priv_gfx_text_plot(x +% col, y +% h -% 1, color);
    }
    var row: i32 = 0;
    while (row < h) : (row += 1) {
        priv_gfx_text_plot(x, y +% row, color);
        priv_gfx_text_plot(x +% w -% 1, y +% row, color);
    }
}

/// `ra8_gfx_init`
pub export fn ra8_gfx_init(fb: ?*anyopaque, width: u16, height: u16, fmt: u8) callconv(.c) u16 {
    const status = impl.initStatus(fb != null, width, height, fmt);
    if (status != impl.err.ok) return status;

    g_gfx_text_state.fb = @ptrCast(fb.?);
    g_gfx_text_state.width = width;
    g_gfx_text_state.height = height;
    g_gfx_text_state.format = fmt;
    g_gfx_text_state.bpp = impl.bppOf(fmt);
    g_gfx_text_state.clip_x0 = 0;
    g_gfx_text_state.clip_y0 = 0;
    g_gfx_text_state.clip_x1 = @intCast(width);
    g_gfx_text_state.clip_y1 = @intCast(height);
    g_gfx_text_state.initialized = true;
    return impl.err.ok;
}

/// `ra8_gfx_clear`
pub export fn ra8_gfx_clear(color: u32) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;

    if (g_gfx_text_state.format == impl.format.rgb565) {
        fillRect565(0, 0, @intCast(g_gfx_text_state.width), @intCast(g_gfx_text_state.height), color);
        return impl.err.ok;
    }

    const fb = g_gfx_text_state.fb orelse return impl.err.ok;
    const row_bytes = stride();
    var y = g_gfx_text_state.clip_y0;
    while (y < g_gfx_text_state.clip_y1) : (y += 1) {
        var x = g_gfx_text_state.clip_x0;
        while (x < g_gfx_text_state.clip_x1) : (x += 1) {
            impl.putPixel(
                fb,
                row_bytes,
                g_gfx_text_state.format,
                @intCast(x),
                @intCast(y),
                color,
            );
        }
    }
    return impl.err.ok;
}

/// `ra8_gfx_set_clip`
pub export fn ra8_gfx_set_clip(x: i32, y: i32, w: i32, h: i32) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;

    const box = impl.clampClip(
        x,
        y,
        w,
        h,
        @intCast(g_gfx_text_state.width),
        @intCast(g_gfx_text_state.height),
    );
    g_gfx_text_state.clip_x0 = box.x0;
    g_gfx_text_state.clip_y0 = box.y0;
    g_gfx_text_state.clip_x1 = box.x1;
    g_gfx_text_state.clip_y1 = box.y1;
    return impl.err.ok;
}

/// `ra8_gfx_reset_clip`
pub export fn ra8_gfx_reset_clip() callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    g_gfx_text_state.clip_x0 = 0;
    g_gfx_text_state.clip_y0 = 0;
    g_gfx_text_state.clip_x1 = @intCast(g_gfx_text_state.width);
    g_gfx_text_state.clip_y1 = @intCast(g_gfx_text_state.height);
    return impl.err.ok;
}

/// `ra8_gfx_pixel`
pub export fn ra8_gfx_pixel(x: i32, y: i32, color: u32) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if (!impl.pixelInBounds(x, y, g_gfx_text_state.width, g_gfx_text_state.height)) {
        return impl.err.range_check_failed;
    }
    priv_gfx_text_plot(x, y, color);
    return impl.err.ok;
}

/// `internal_blit_gray8_565`
fn blitGray8Fast(src: [*]const u8, w: i32, dst_x: i32, dst_y: i32, box: impl.Box) void {
    const fb = g_gfx_text_state.fb orelse return;
    const row_bytes = stride();
    const src_w: usize = @intCast(w);

    var y = box.y0;
    while (y < box.y1) : (y += 1) {
        var p = fb + (@as(usize, @intCast(y)) * row_bytes) + (@as(usize, @intCast(box.x0)) * impl.rgb565_bpp);
        var s = src + (@as(usize, @intCast(y - dst_y)) * src_w) + @as(usize, @intCast(box.x0 - dst_x));
        var x = box.x0;
        while (x < box.x1) : (x += 1) {
            const v = impl.pack565(impl.grayToColor(s[0]));
            p[0] = @intCast(v & 0xFF);
            p[1] = @intCast((v >> 8) & 0xFF);
            p += impl.rgb565_bpp;
            s += 1;
        }
    }
}

/// `internal_blit_gray8_slow`
fn blitGray8Slow(src: [*]const u8, w: i32, h: i32, dx: i32, dy: i32) void {
    const src_w: usize = @intCast(w);
    var y: i32 = 0;
    while (y < h) : (y += 1) {
        var x: i32 = 0;
        while (x < w) : (x += 1) {
            const sample = src[(@as(usize, @intCast(y)) * src_w) + @as(usize, @intCast(x))];
            priv_gfx_text_plot(dx +% x, dy +% y, impl.grayToColor(sample));
        }
    }
}

/// `ra8_gfx_blit_gray8`
pub export fn ra8_gfx_blit_gray8(
    src: ?[*]const u8,
    w: i32,
    h: i32,
    dst_x: i32,
    dst_y: i32,
) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if (!impl.blitGray8ArgsOk(src != null, w, h)) return impl.err.invalid_arg;
    const pixels = src.?;

    if (g_gfx_text_state.format != impl.format.rgb565) {
        blitGray8Slow(pixels, w, h, dst_x, dst_y);
        return impl.err.ok;
    }

    if (impl.blitWindow(dst_x, dst_y, w, h, clip())) |box| {
        blitGray8Fast(pixels, w, dst_x, dst_y, box);
    }
    return impl.err.ok;
}

/// `ra8_gfx_line`
pub export fn ra8_gfx_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;

    var walk = impl.lineStart(x0, y0, x1, y1);
    var i: i32 = 0;
    while (i < impl.Line.max_iterations) : (i += 1) {
        priv_gfx_text_plot(walk.x, walk.y, color);
        if ((walk.x == x1) and (walk.y == y1)) break;
        walk = impl.lineStep(walk);
    }
    return impl.err.ok;
}

/// `ra8_gfx_rect`
pub export fn ra8_gfx_rect(
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    color: u32,
    filled: bool,
) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if ((w <= 0) or (h <= 0)) return impl.err.ok;
    if (filled) {
        fillRect(x, y, w, h, color);
    } else {
        rectOutline(x, y, w, h, color);
    }
    return impl.err.ok;
}

/// `internal_circle_outline_step`: the eight-way octant reflection.
fn circleOutlineStep(cx: i32, cy: i32, x: i32, y: i32, color: u32) void {
    priv_gfx_text_plot(cx +% x, cy +% y, color);
    priv_gfx_text_plot(cx -% x, cy +% y, color);
    priv_gfx_text_plot(cx +% x, cy -% y, color);
    priv_gfx_text_plot(cx -% x, cy -% y, color);
    priv_gfx_text_plot(cx +% y, cy +% x, color);
    priv_gfx_text_plot(cx -% y, cy +% x, color);
    priv_gfx_text_plot(cx +% y, cy -% x, color);
    priv_gfx_text_plot(cx -% y, cy -% x, color);
}

/// `internal_circle_filled_step`: the two mirrored spans. The counters are
/// widened to 64-bit so a degenerate radius cannot wrap the loop itself,
/// while the plotted coordinates keep the C's 32-bit arithmetic.
fn circleFilledStep(cx: i32, cy: i32, x: i32, y: i32, color: u32) void {
    var col: i64 = -@as(i64, x);
    while (col <= @as(i64, x)) : (col += 1) {
        const c: i32 = @truncate(col);
        priv_gfx_text_plot(cx +% c, cy +% y, color);
        priv_gfx_text_plot(cx +% c, cy -% y, color);
    }
    col = -@as(i64, y);
    while (col <= @as(i64, y)) : (col += 1) {
        const c: i32 = @truncate(col);
        priv_gfx_text_plot(cx +% c, cy +% x, color);
        priv_gfx_text_plot(cx +% c, cy -% x, color);
    }
}

/// `ra8_gfx_circle`
pub export fn ra8_gfx_circle(
    cx: i32,
    cy: i32,
    r: i32,
    color: u32,
    filled: bool,
) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if (r < 0) return impl.err.invalid_arg;

    var walk = impl.circleStart(r);
    var i: i32 = 0;
    while (i < impl.Circle.max_iterations) : (i += 1) {
        if (filled) {
            circleFilledStep(cx, cy, walk.x, walk.y, color);
        } else {
            circleOutlineStep(cx, cy, walk.x, walk.y, color);
        }
        if (walk.done()) break;
        walk = impl.circleStep(walk);
    }
    return impl.err.ok;
}

/// `ra8_gfx_blit`
pub export fn ra8_gfx_blit(
    src_buf: ?*const anyopaque,
    src_w: u16,
    src_h: u16,
    src_format: u8,
    dst_x: i32,
    dst_y: i32,
) callconv(.c) u16 {
    if (src_buf == null) return impl.err.null_ptr;
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if (!impl.blitArgsOk(src_w, src_h, src_format)) return impl.err.invalid_arg;

    const src: [*]const u8 = @ptrCast(src_buf.?);
    const src_stride = @as(usize, src_w) * @as(usize, impl.bppOf(src_format));

    var row: u32 = 0;
    while (row < src_h) : (row += 1) {
        var col: u32 = 0;
        while (col < src_w) : (col += 1) {
            const color = impl.getPixel(src, src_stride, src_format, col, row);
            priv_gfx_text_plot(
                dst_x +% @as(i32, @intCast(col)),
                dst_y +% @as(i32, @intCast(row)),
                color,
            );
        }
    }
    return impl.err.ok;
}

/// `internal_gray4_color`
fn gray4Color(src: [*]const u8, src_w: i32, x: i32, y: i32) u32 {
    const flat = impl.gray4FlatIndex(src_w, x, y);
    const nibble = impl.gray4Nibble(src[flat >> 1], flat);
    return impl.grayToColor(@as(u32, impl.gray4ToGray8(nibble)));
}

/// `internal_gray4_block`
fn gray4Block(bx: i32, by: i32, zoom: i32, color: u32) void {
    var dy: i32 = 0;
    while (dy < zoom) : (dy += 1) {
        var dx: i32 = 0;
        while (dx < zoom) : (dx += 1) {
            priv_gfx_text_plot(bx +% dx, by +% dy, color);
        }
    }
}

/// `ra8_gfx_blit_gray4_zoom`
pub export fn ra8_gfx_blit_gray4_zoom(
    src: ?[*]const u8,
    src_w: i32,
    src_h: i32,
    sx: i32,
    sy: i32,
    sw: i32,
    sh: i32,
    zoom: i32,
    dst_x: i32,
    dst_y: i32,
) callconv(.c) u16 {
    if (!g_gfx_text_state.initialized) return impl.err.not_initialized;
    if (!impl.gray4ZoomArgsOk(src != null, zoom, src_w, src_h)) return impl.err.invalid_arg;
    const pixels = src.?;

    const window = impl.gray4Window(sx, sy, sw, sh, src_w, src_h);
    var py = window.y0;
    while (py < window.y1) : (py += 1) {
        const by = dst_y +% ((py -% sy) *% zoom);
        var px = window.x0;
        while (px < window.x1) : (px += 1) {
            gray4Block(
                dst_x +% ((px -% sx) *% zoom),
                by,
                zoom,
                gray4Color(pixels, src_w, px, py),
            );
        }
    }
    return impl.err.ok;
}
