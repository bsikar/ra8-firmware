//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure, side-effect-free core of the `ra8_gfx` software rasteriser: colour
//! packing and unpacking, format classification, the clip-rectangle algebra
//! shared by every draw entry point, and the Bresenham line and midpoint
//! circle steppers. Nothing here reads module state or calls out of the
//! library, so every branch is reachable from a host unit test.
//!
//! The framebuffer binding itself (`g_gfx_text_state`) and the exported C
//! entry points live in `../ra8_gfx_abi.zig`.

const std = @import("std");

/// `ra8_err_t` values this library can return.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const not_initialized: u16 = 0x10F;
    pub const range_check_failed: u16 = 0x503;
    pub const null_ptr: u16 = 0x504;
};

/// `ra8_gfx_format_t`. The numeric value IS the bytes-per-pixel, which the C
/// relied on through `internal_bpp()` returning the enumerator unchanged.
pub const format = struct {
    pub const rgb565: u8 = 2;
    pub const rgb888: u8 = 3;
    pub const argb8888: u8 = 4;
};

/// `ra8_gfx_dim_limits_t`.
pub const dim = struct {
    pub const min: u16 = 1;
    pub const max: u16 = 4096;
};

const shift_blue: u5 = 0;
const shift_green: u5 = 8;
const shift_red: u5 = 16;
const shift_alpha: u5 = 24;
const mask_byte: u32 = 0xFF;

const r565_mask: u32 = 0x1F;
const g565_mask: u32 = 0x3F;
const b565_mask: u32 = 0x1F;
const r565_shift_in: u5 = 3;
const g565_shift_in: u5 = 2;
const b565_shift_in: u5 = 3;
const r565_shift_out: u5 = 11;
const g565_shift_out: u5 = 5;

/// Bytes per pixel of the RGB565 fast paths, spelled out where the C did.
pub const rgb565_bpp: usize = 2;

/// `ra8_gfx_state_t` from `src/ra8_gfx_internal.h`, the single module-wide
/// framebuffer binding. Four C translation units still include that header and
/// read these fields, so the layout is ABI and is pinned below.
pub const State = extern struct {
    fb: ?[*]u8 = null,
    width: u16 = 0,
    height: u16 = 0,
    format: u8 = format.rgb565,
    bpp: u8 = 0,
    initialized: bool = false,
    clip_x0: i32 = 0,
    clip_y0: i32 = 0,
    clip_x1: i32 = 0,
    clip_y1: i32 = 0,
};

comptime {
    const ptr = @sizeOf(usize);
    // fb, then the four packed byte-ish fields, then the clip box realigned
    // to 4. Written as alignment arithmetic so the assert holds on both the
    // 8-byte-pointer host and 32-bit Arm, where `ptr * N` does not.
    const clip_at = std.mem.alignForward(usize, ptr + 7, 4);
    std.debug.assert(@offsetOf(State, "fb") == 0);
    std.debug.assert(@offsetOf(State, "width") == ptr);
    std.debug.assert(@offsetOf(State, "height") == ptr + 2);
    std.debug.assert(@offsetOf(State, "format") == ptr + 4);
    std.debug.assert(@offsetOf(State, "bpp") == ptr + 5);
    std.debug.assert(@offsetOf(State, "initialized") == ptr + 6);
    std.debug.assert(@offsetOf(State, "clip_x0") == clip_at);
    std.debug.assert(@offsetOf(State, "clip_y0") == clip_at + 4);
    std.debug.assert(@offsetOf(State, "clip_x1") == clip_at + 8);
    std.debug.assert(@offsetOf(State, "clip_y1") == clip_at + 12);
    std.debug.assert(@sizeOf(State) == clip_at + 16);
}

/// Red channel of a 0xAARRGGBB colour.
pub fn colorR(color: u32) u8 {
    return @intCast((color >> shift_red) & mask_byte);
}

/// Green channel of a 0xAARRGGBB colour.
pub fn colorG(color: u32) u8 {
    return @intCast((color >> shift_green) & mask_byte);
}

/// Blue channel of a 0xAARRGGBB colour.
pub fn colorB(color: u32) u8 {
    return @intCast((color >> shift_blue) & mask_byte);
}

/// Alpha channel of a 0xAARRGGBB colour.
pub fn colorA(color: u32) u8 {
    return @intCast((color >> shift_alpha) & mask_byte);
}

/// Pack a 0xAARRGGBB colour into one RGB565 word (`priv_gfx_text_pack_565`).
pub fn pack565(color: u32) u16 {
    const r: u16 = @as(u16, colorR(color)) >> r565_shift_in;
    const g: u16 = @as(u16, colorG(color)) >> g565_shift_in;
    const b: u16 = @as(u16, colorB(color)) >> b565_shift_in;
    return (r << r565_shift_out) | (g << g565_shift_out) | b;
}

/// Expand an RGB565 word back to 0x00RRGGBB, the way `internal_get_pixel` did.
pub fn unpack565(word: u16) u32 {
    const v: u32 = word;
    const r = ((v >> r565_shift_out) & r565_mask) << r565_shift_in;
    const g = ((v >> g565_shift_out) & g565_mask) << g565_shift_in;
    const b = (v & b565_mask) << b565_shift_in;
    return (r << shift_red) | (g << shift_green) | (b << shift_blue);
}

/// Bytes per pixel of a format: the C returned the enumerator unchanged.
pub fn bppOf(fmt: u8) u8 {
    return fmt;
}

/// True for the three formats the library can address.
pub fn formatOk(fmt: u8) bool {
    return (fmt == format.rgb565) or (fmt == format.rgb888) or (fmt == format.argb8888);
}

/// Replicate an 8-bit gray level across R, G and B.
pub fn grayToColor(g: u32) u32 {
    return (g << shift_red) | (g << shift_green) | (g << shift_blue);
}

/// Guard order of `ra8_gfx_init`: framebuffer, width, height, then format.
pub fn initStatus(has_fb: bool, width: u16, height: u16, fmt: u8) u16 {
    if (!has_fb) return err.null_ptr;
    if ((width < dim.min) or (width > dim.max)) return err.invalid_arg;
    if ((height < dim.min) or (height > dim.max)) return err.invalid_arg;
    if (!formatOk(fmt)) return err.invalid_arg;
    return err.ok;
}

/// A half-open pixel box: x0/y0 inclusive, x1/y1 exclusive.
pub const Box = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,

    pub fn isEmpty(self: Box) bool {
        return (self.x1 <= self.x0) or (self.y1 <= self.y0);
    }
};

/// `ra8_gfx_set_clip`: clamp the requested rectangle into the framebuffer,
/// widening the right and bottom edges in 64-bit so `x + w` cannot wrap, then
/// collapsing an inverted box onto its own origin.
pub fn clampClip(x: i32, y: i32, w: i32, h: i32, fb_w: i32, fb_h: i32) Box {
    var x0 = if (x > 0) x else 0;
    var y0 = if (y > 0) y else 0;
    if (x0 > fb_w) x0 = fb_w;
    if (y0 > fb_h) y0 = fb_h;

    const xr = @as(i64, x) + @as(i64, w);
    const yr = @as(i64, y) + @as(i64, h);
    var x1: i32 = if (xr < @as(i64, fb_w)) @intCast(xr) else fb_w;
    var y1: i32 = if (yr < @as(i64, fb_h)) @intCast(yr) else fb_h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;

    return .{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
}

/// `internal_fill_rect_565`'s intersection of a rectangle with the clip box.
/// Null means nothing is drawable, which is the C's early return.
pub fn fillSpan(x: i32, y: i32, w: i32, h: i32, clip: Box) ?Box {
    const x0 = if (x >= clip.x0) x else clip.x0;
    const y0 = if (y >= clip.y0) y else clip.y0;
    const xr = @as(i64, x) + @as(i64, w);
    const yr = @as(i64, y) + @as(i64, h);
    const x1: i32 = if (xr < @as(i64, clip.x1)) @intCast(xr) else clip.x1;
    const y1: i32 = if (yr < @as(i64, clip.y1)) @intCast(yr) else clip.y1;
    const box = Box{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
    return if (box.isEmpty()) null else box;
}

/// `ra8_gfx_blit_gray8`'s RGB565 window. The C adds `dst_x + w` in `int32_t`,
/// so the wrap is kept rather than promoted, and the empty case is null.
pub fn blitWindow(dst_x: i32, dst_y: i32, w: i32, h: i32, clip: Box) ?Box {
    const x0 = if (dst_x > clip.x0) dst_x else clip.x0;
    const y0 = if (dst_y > clip.y0) dst_y else clip.y0;
    const xw = dst_x +% w;
    const yh = dst_y +% h;
    const x1 = if (xw < clip.x1) xw else clip.x1;
    const y1 = if (yh < clip.y1) yh else clip.y1;
    if ((x0 < x1) and (y0 < y1)) {
        return .{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
    }
    return null;
}

/// `ra8_gfx_pixel`'s framebuffer bounds check, which is judged against the
/// full surface and NOT against the clip box.
pub fn pixelInBounds(x: i32, y: i32, width: u16, height: u16) bool {
    if ((x < 0) or (y < 0)) return false;
    return (x < @as(i32, width)) and (y < @as(i32, height));
}

/// `priv_gfx_text_plot`'s clip test, in the C's two-comparison order.
pub fn plotInClip(x: i32, y: i32, clip: Box) bool {
    if ((x < clip.x0) or (y < clip.y0)) return false;
    if ((x >= clip.x1) or (y >= clip.y1)) return false;
    return true;
}

/// True when a packed RGB565 word has identical halves, so the row fill can
/// be one `memset` instead of an interleaving loop.
pub fn fillIsFlat(lo: u8, hi: u8) bool {
    return lo == hi;
}

/// Byte offset of pixel (x, y) in a surface of the given stride and depth.
pub fn pixelOffset(stride: usize, bpp: usize, x: usize, y: usize) usize {
    return (y * stride) + (x * bpp);
}

/// Write one pixel into `dst` in the given format (`internal_put_pixel`). An
/// unrecognised format writes nothing, matching the C switch with no default.
pub fn putPixel(dst: [*]u8, stride: usize, fmt: u8, x: usize, y: usize, color: u32) void {
    const p = dst + pixelOffset(stride, bppOf(fmt), x, y);
    switch (fmt) {
        format.rgb565 => {
            const v = pack565(color);
            p[0] = @intCast(v & 0xFF);
            p[1] = @intCast((v >> 8) & 0xFF);
        },
        format.rgb888 => {
            p[0] = colorR(color);
            p[1] = colorG(color);
            p[2] = colorB(color);
        },
        format.argb8888 => {
            p[0] = colorB(color);
            p[1] = colorG(color);
            p[2] = colorR(color);
            p[3] = colorA(color);
        },
        else => {},
    }
}

/// Read one pixel out of `src` in the given format (`internal_get_pixel`). An
/// unrecognised format reads as 0, which is the C's trailing `return 0U`.
pub fn getPixel(src: [*]const u8, stride: usize, fmt: u8, x: usize, y: usize) u32 {
    const p = src + pixelOffset(stride, bppOf(fmt), x, y);
    switch (fmt) {
        format.rgb565 => {
            const word: u16 = @as(u16, p[0]) | (@as(u16, p[1]) << 8);
            return unpack565(word);
        },
        format.rgb888 => {
            const r: u32 = p[0];
            const g: u32 = p[1];
            const b: u32 = p[2];
            return (r << shift_red) | (g << shift_green) | (b << shift_blue);
        },
        format.argb8888 => {
            const b: u32 = p[0];
            const g: u32 = p[1];
            const r: u32 = p[2];
            const a: u32 = p[3];
            return (a << shift_alpha) | (r << shift_red) | (g << shift_green) | b;
        },
        else => return 0,
    }
}

/// Bresenham line state.
pub const Line = struct {
    x: i32,
    y: i32,
    e: i32,
    dx: i32,
    dy: i32,
    sx: i32,
    sy: i32,

    /// Iteration ceiling the C used instead of trusting the endpoints.
    pub const max_iterations: i32 = @as(i32, dim.max) * 2;
};

/// Seed a line walk from its endpoints, keeping the C's absolute-difference
/// spelling (`dy` is the negated vertical span).
pub fn lineStart(x0: i32, y0: i32, x1: i32, y1: i32) Line {
    const dx = if (x1 >= x0) x1 -% x0 else x0 -% x1;
    const dy = if (y1 >= y0) -%(y1 -% y0) else -%(y0 -% y1);
    return .{
        .x = x0,
        .y = y0,
        .e = dx +% dy,
        .dx = dx,
        .dy = dy,
        .sx = if (x0 < x1) 1 else -1,
        .sy = if (y0 < y1) 1 else -1,
    };
}

/// Advance a line walk one step. Both branches can fire on the same step,
/// which is what makes the walk diagonal.
pub fn lineStep(state: Line) Line {
    var next = state;
    const e2 = state.e *% 2;
    if (e2 >= state.dy) {
        next.e +%= state.dy;
        next.x +%= state.sx;
    }
    if (e2 <= state.dx) {
        next.e +%= state.dx;
        next.y +%= state.sy;
    }
    return next;
}

/// Midpoint circle state.
pub const Circle = struct {
    x: i32,
    y: i32,
    e: i32,

    /// Iteration ceiling the C used.
    pub const max_iterations: i32 = @as(i32, dim.max);

    /// The walk stops once the octant closes.
    pub fn done(self: Circle) bool {
        return self.x <= self.y;
    }
};

/// Seed a circle walk of radius `r`.
pub fn circleStart(r: i32) Circle {
    return .{ .x = r, .y = 0, .e = 1 -% r };
}

/// Advance a circle walk one step.
pub fn circleStep(state: Circle) Circle {
    var next = state;
    next.y +%= 1;
    if (state.e < 0) {
        next.e +%= (2 *% next.y) +% 1;
    } else {
        next.x -%= 1;
        next.e +%= (2 *% (next.y -% next.x)) +% 1;
    }
    return next;
}

/// Guard order of `ra8_gfx_blit_gray8` once the init check has passed.
pub fn blitGray8ArgsOk(has_src: bool, w: i32, h: i32) bool {
    return has_src and (w > 0) and (h > 0);
}

/// Guard order of `ra8_gfx_blit` once null and init have been judged.
pub fn blitArgsOk(src_w: u16, src_h: u16, src_format: u8) bool {
    return (src_w != 0) and (src_h != 0) and formatOk(src_format);
}

// ---------------------------------------------------------------------------
// Packed gray4 sampling for the reader-loupe zoom blit
// ---------------------------------------------------------------------------

/// Low-nibble mask of the packed gray4 format (`k_ra8_g4_nib_lo`).
pub const gray4_nibble_mask: u8 = 0x0F;

/// Nibble shift, which is also the 4-bit to 8-bit replicate (`k_ra8_g4_nib_sh`).
const gray4_nibble_shift: u3 = 4;

/// Flat nibble index of source pixel (`x`, `y`) in a `src_w`-wide packed image.
///
/// Two pixels share a byte, so the containing byte is `flat >> 1` and the
/// parity of `flat` picks the half. Both coordinates are already clamped into
/// the image by `gray4Window`, so the widening is in range.
pub fn gray4FlatIndex(src_w: i32, x: i32, y: i32) usize {
    return (@as(usize, @intCast(y)) * @as(usize, @intCast(src_w))) + @as(usize, @intCast(x));
}

/// Select the gray4 level at flat index `flat` out of its containing byte:
/// the high nibble for an even index, the low nibble for an odd one.
pub fn gray4Nibble(byte: u8, flat: usize) u8 {
    return if ((flat & 1) != 0) (byte & gray4_nibble_mask) else (byte >> gray4_nibble_shift);
}

/// Replicate a 4-bit level into 8 bits, `(n << 4) | n`, the same expansion
/// `ra8_gfx_blit_gray8` consumers and the dither packer use.
///
/// The mask is a no-op on every value `gray4Nibble` can return (both halves
/// are already four bits); it is spelled out so the shift cannot overflow.
pub fn gray4ToGray8(nibble: u8) u8 {
    const n: u8 = nibble & gray4_nibble_mask;
    return (n << gray4_nibble_shift) | n;
}

/// The source sub-rectangle a gray4 zoom blit actually samples, clamped to the
/// image bounds. Half-open on both axes: `x0 <= x < x1`, `y0 <= y < y1`.
pub const Gray4Window = struct {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,

    /// True when the clamp collapsed the window, so nothing is drawn.
    pub fn isEmpty(self: Gray4Window) bool {
        return (self.x0 >= self.x1) or (self.y0 >= self.y1);
    }
};

/// Clamp the requested sub-rectangle to the source image.
///
/// An off-image edge draws only its in-image portion, still at its natural
/// `dst + offset * zoom` position, and a non-positive `sw`/`sh` collapses the
/// range so nothing is drawn. The far edges are summed with wrapping
/// arithmetic because the C's `sx + sw` was plain `int32_t` addition.
pub fn gray4Window(sx: i32, sy: i32, sw: i32, sh: i32, src_w: i32, src_h: i32) Gray4Window {
    const x_hi = sx +% sw;
    const y_hi = sy +% sh;
    return .{
        .x0 = if (sx > 0) sx else 0,
        .y0 = if (sy > 0) sy else 0,
        .x1 = if (x_hi < src_w) x_hi else src_w,
        .y1 = if (y_hi < src_h) y_hi else src_h,
    };
}

/// The argument chain of `ra8_gfx_blit_gray4_zoom` after the init check: a
/// buffer, a positive zoom, then a non-empty source. Every one of the C's
/// three guards returned `invalid_arg`, so they collapse into one predicate.
pub fn gray4ZoomArgsOk(has_src: bool, zoom: i32, src_w: i32, src_h: i32) bool {
    if (!has_src) return false;
    if (zoom <= 0) return false;
    return (src_w > 0) and (src_h > 0);
}
