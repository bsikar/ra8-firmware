//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Viewport clipping and dual-path (HW ops / bit-exact RGB565 SW fallback)
//! band compositor for `longstrip`. Keeps zero dependency on HAL/board layers
//! so host tools (`rabook_viewer`) and EK-RA8D2 DRW firmware share one contract.

const ls_h = @import("longstrip_h");

/// Supported source bytes-per-pixel widths matching JOF and ra8_drw_mve.
pub const Bpp = enum(u8) {
    gray8 = 1,
    rgb565 = 2,
    rgb888 = 3,
    rgba8888 = 4,

    pub fn fromU8(raw: u8) ?Bpp {
        return switch (raw) {
            1 => .gray8,
            2 => .rgb565,
            3 => .rgb888,
            4 => .rgba8888,
            else => null,
        };
    }
};

/// Pack 8-bit R, G, B channels into little-endian RGB565 matching ra8_drw_mve and ra8_gfx.
pub inline fn packRgb565(r: u8, g: u8, b: u8) u16 {
    const r5: u16 = @as(u16, r >> 3) << 11;
    const g6: u16 = @as(u16, g >> 2) << 5;
    const b5: u16 = @as(u16, b >> 3);
    return r5 | g6 | b5;
}

/// Unpack a 32-bit 0xAARRGGBB color word into RGB565.
pub inline fn argb8888ToRgb565(argb: u32) u16 {
    const r: u8 = @truncate(argb >> 16);
    const g: u8 = @truncate(argb >> 8);
    const b: u8 = @truncate(argb);
    return packRgb565(r, g, b);
}

/// Compute exact viewport clip rectangle `[0, view_w) x [0, view_h)`.
pub fn clipBand(
    dst_x: i32,
    dst_y: i32,
    src_w: u16,
    src_h: u16,
    view_w: u16,
    view_h: u16,
    out: *ls_h.longstrip_clip_rect_t,
) ls_h.ra8_err_t {
    if (src_w == 0 or src_h == 0 or view_w == 0 or view_h == 0) {
        out.* = .{
            .src_x = 0,
            .src_y = 0,
            .dst_x = 0,
            .dst_y = 0,
            .width = 0,
            .height = 0,
            .visible = false,
        };
        return ls_h.k_ra8_err_invalid_arg;
    }

    const sx0: i64 = dst_x;
    const sy0: i64 = dst_y;
    const sx1: i64 = sx0 + @as(i64, src_w);
    const sy1: i64 = sy0 + @as(i64, src_h);

    const cx0: i64 = @max(sx0, 0);
    const cy0: i64 = @max(sy0, 0);
    const cx1: i64 = @min(sx1, @as(i64, view_w));
    const cy1: i64 = @min(sy1, @as(i64, view_h));

    if (cx0 >= cx1 or cy0 >= cy1) {
        out.* = .{
            .src_x = 0,
            .src_y = 0,
            .dst_x = 0,
            .dst_y = 0,
            .width = 0,
            .height = 0,
            .visible = false,
        };
        return ls_h.k_ra8_ok;
    }

    out.* = .{
        .src_x = @intCast(cx0 - sx0),
        .src_y = @intCast(cy0 - sy0),
        .dst_x = @intCast(cx0),
        .dst_y = @intCast(cy0),
        .width = @intCast(cx1 - cx0),
        .height = @intCast(cy1 - cy0),
        .visible = true,
    };
    return ls_h.k_ra8_ok;
}

fn convertRowToRgb565(
    src_row: [*]const u8,
    dst_row: [*]u16,
    width: usize,
    fmt: Bpp,
) void {
    switch (fmt) {
        .gray8 => {
            var x: usize = 0;
            while (x < width) : (x += 1) {
                const y = src_row[x];
                dst_row[x] = packRgb565(y, y, y);
            }
        },
        .rgb565 => {
            var x: usize = 0;
            while (x < width) : (x += 1) {
                const i = x * 2;
                dst_row[x] = @as(u16, src_row[i]) | (@as(u16, src_row[i + 1]) << 8);
            }
        },
        .rgb888 => {
            var x: usize = 0;
            while (x < width) : (x += 1) {
                const i = x * 3;
                dst_row[x] = packRgb565(src_row[i], src_row[i + 1], src_row[i + 2]);
            }
        },
        .rgba8888 => {
            var x: usize = 0;
            while (x < width) : (x += 1) {
                const i = x * 4;
                dst_row[x] = packRgb565(src_row[i], src_row[i + 1], src_row[i + 2]);
            }
        },
    }
}

/// Software RGB565 compositor with bit-exact parity against ra8_drw + MVE.
pub fn swBlitRgb565(
    fb565: [*]u16,
    fb_pitch_px: u16,
    view_w: u16,
    view_h: u16,
    pixels: [*]const u8,
    src_w: u16,
    src_h: u16,
    bpp_raw: u8,
    dst_x: i32,
    dst_y: i32,
) ls_h.ra8_err_t {
    const fmt = Bpp.fromU8(bpp_raw) orelse return ls_h.k_ra8_err_invalid_arg;
    if (fb_pitch_px < view_w) {
        return ls_h.k_ra8_err_invalid_arg;
    }

    var clip: ls_h.longstrip_clip_rect_t = undefined;
    const clip_rc = clipBand(dst_x, dst_y, src_w, src_h, view_w, view_h, &clip);
    if (clip_rc != ls_h.k_ra8_ok) {
        return clip_rc;
    }
    if (!clip.visible) {
        return ls_h.k_ra8_ok;
    }

    const bpp_usize: usize = @backingInt(fmt);
    const sw_usize: usize = src_w;
    const pitch_usize: usize = fb_pitch_px;
    const cw: usize = clip.width;
    const ch: usize = clip.height;
    const dx_usize: usize = @intCast(clip.dst_x);
    const dy_usize: usize = @intCast(clip.dst_y);

    var r: usize = 0;
    while (r < ch) : (r += 1) {
        const sy_usize: usize = @as(usize, clip.src_y) + r;
        const sx_usize: usize = @as(usize, clip.src_x);
        const s_off = (sy_usize * sw_usize + sx_usize) * bpp_usize;
        const d_off = (dy_usize + r) * pitch_usize + dx_usize;
        convertRowToRgb565(pixels + s_off, fb565 + d_off, cw, fmt);
    }
    return ls_h.k_ra8_ok;
}

/// Software clipped solid RGB565 rectangle fill.
pub fn swFillRectRgb565(
    fb565: [*]u16,
    fb_pitch_px: u16,
    view_w: u16,
    view_h: u16,
    x: i32,
    y: i32,
    w: u16,
    h: u16,
    argb8888: u32,
) ls_h.ra8_err_t {
    if (fb_pitch_px < view_w) {
        return ls_h.k_ra8_err_invalid_arg;
    }
    var clip: ls_h.longstrip_clip_rect_t = undefined;
    const clip_rc = clipBand(x, y, w, h, view_w, view_h, &clip);
    if (clip_rc != ls_h.k_ra8_ok) {
        return clip_rc;
    }
    if (!clip.visible) {
        return ls_h.k_ra8_ok;
    }

    const px565 = argb8888ToRgb565(argb8888);
    const cw: usize = clip.width;
    const ch: usize = clip.height;
    const dx_usize: usize = @intCast(clip.dst_x);
    const dy_usize: usize = @intCast(clip.dst_y);
    const pitch_usize: usize = fb_pitch_px;

    var r: usize = 0;
    while (r < ch) : (r += 1) {
        const row_ptr = fb565 + (dy_usize + r) * pitch_usize + dx_usize;
        @memset(row_ptr[0..cw], px565);
    }
    return ls_h.k_ra8_ok;
}

/// Target blit router: dispatches to HW ops when preferred and wired, else SW RGB565.
pub fn targetBlit(
    target: *const ls_h.longstrip_target_t,
    pixels: [*]const u8,
    src_w: u16,
    src_h: u16,
    bpp: u8,
    dst_x: i32,
    dst_y: i32,
) ls_h.ra8_err_t {
    const shifted_x: i32 = @intCast(@max(@as(i64, -2147483648), @min(@as(i64, 2147483647), @as(i64, dst_x) + @as(i64, target.x_off))));
    if (target.prefer_hw) {
        if (target.hw.blit_band) |hw_fn| {
            return hw_fn(target.hw.ctx, pixels, src_w, src_h, bpp, shifted_x, dst_y, target.view_w, target.view_h);
        }
    }
    const fb = target.fb565 orelse return ls_h.k_ra8_err_null_ptr;
    return swBlitRgb565(fb, target.fb_pitch_px, target.view_w, target.view_h, pixels, src_w, src_h, bpp, shifted_x, dst_y);
}

/// Target solid fill router: dispatches to HW fill_rect when preferred and wired, else SW RGB565.
pub fn targetFill(
    target: *const ls_h.longstrip_target_t,
    x: i32,
    y: i32,
    w: u16,
    h: u16,
    argb8888: u32,
) ls_h.ra8_err_t {
    if (w == 0 or h == 0 or target.view_w == 0 or target.view_h == 0) {
        return ls_h.k_ra8_err_invalid_arg;
    }
    if (target.prefer_hw) {
        if (target.hw.fill_rect) |hw_fill| {
            var clip: ls_h.longstrip_clip_rect_t = undefined;
            const rc = clipBand(x, y, w, h, target.view_w, target.view_h, &clip);
            if (rc != ls_h.k_ra8_ok) return rc;
            if (!clip.visible) return ls_h.k_ra8_ok;
            return hw_fill(target.hw.ctx, clip.dst_x, clip.dst_y, clip.width, clip.height, argb8888);
        }
    }
    const fb = target.fb565 orelse return ls_h.k_ra8_err_null_ptr;
    return swFillRectRgb565(fb, target.fb_pitch_px, target.view_w, target.view_h, x, y, w, h, argb8888);
}
