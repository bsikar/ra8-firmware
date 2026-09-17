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
    pub const no_mem: u16 = 0x102;
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

// ---------------------------------------------------------------------------
// Monochrome glyph cells and text layout
// ---------------------------------------------------------------------------

/// Glyph bits per byte (`k_glyph_bits_per_byte`).
pub const glyph_bits_per_byte: u32 = 8;

/// Index of the leftmost bit inside a glyph byte (`k_glyph_msb_index`).
pub const glyph_msb_index: u32 = 7;

/// `ra8_gfx_font_t` from `inc/ra8_gfx_font.h`. Callers pass a pointer to one of
/// these and the bundled 8x16 table is still a C object, so this layout is ABI
/// and is pinned below. `-fshort-enums` cannot reach it: every field after the
/// pointer is a plain `uint8_t`.
pub const Font = extern struct {
    glyph_data: ?[*]const u8 = null,
    glyph_width: u8 = 0,
    glyph_height: u8 = 0,
    bytes_per_glyph: u8 = 0,
    first_codepoint: u8 = 0,
    last_codepoint: u8 = 0,
};

comptime {
    const ptr = @sizeOf(usize);
    // Alignment arithmetic rather than `ptr * N`, so the assert holds on the
    // 8-byte-pointer host and on 32-bit Arm alike.
    std.debug.assert(@offsetOf(Font, "glyph_data") == 0);
    std.debug.assert(@offsetOf(Font, "glyph_width") == ptr);
    std.debug.assert(@offsetOf(Font, "glyph_height") == ptr + 1);
    std.debug.assert(@offsetOf(Font, "bytes_per_glyph") == ptr + 2);
    std.debug.assert(@offsetOf(Font, "first_codepoint") == ptr + 3);
    std.debug.assert(@offsetOf(Font, "last_codepoint") == ptr + 4);
    std.debug.assert(@sizeOf(Font) == std.mem.alignForward(usize, ptr + 5, ptr));
}

/// Ceiling on the glyphs one text call walks. The C bounded both text loops by
/// `k_ra8_gfx_max_dim` ("at most one glyph per pixel column we could ever
/// cover") instead of trusting the string to terminate.
pub const max_chars: u32 = dim.max;

/// Bytes one glyph row occupies: `ceil(glyph_width / 8)`.
pub fn glyphRowBytes(glyph_width: u8) u32 {
    return (@as(u32, glyph_width) + (glyph_bits_per_byte - 1)) / glyph_bits_per_byte;
}

/// Glyph slot of a codepoint. A codepoint outside the stored range renders as
/// slot 0, which is the space in every bundled font.
pub fn glyphIndex(cp: u8, first: u8, last: u8) u8 {
    if ((cp < first) or (cp > last)) return 0;
    return cp - first;
}

/// Byte offset of glyph slot `idx` inside the packed glyph table.
pub fn glyphDataOffset(idx: u8, bytes_per_glyph: u8) usize {
    return @as(usize, idx) * @as(usize, bytes_per_glyph);
}

/// Byte index within one glyph cell of the pixel at (`col`, `row`). The C did
/// this in `uint32_t`, so the wrap is kept; both inputs are bounded by the
/// glyph geometry, which is `uint8_t`-derived.
pub fn glyphByteIndex(row_bytes: u32, row: u32, col: u32) usize {
    return @as(usize, (row *% row_bytes) +% (col / glyph_bits_per_byte));
}

/// True when the glyph bit at (`col`, `row`) is set. Bits are MSB-first within
/// each byte, so column 0 is bit 7 and every row restarts on a byte boundary.
pub fn glyphBitSet(gd: [*]const u8, row_bytes: u32, row: u32, col: u32) bool {
    const bit: u3 = @intCast(glyph_msb_index - (col % glyph_bits_per_byte));
    return ((gd[glyphByteIndex(row_bytes, row, col)] >> bit) & 0x01) != 0;
}

/// The on-screen span of one glyph cell: the cell intersected with the active
/// clip box. Null is the C's "glyph fully outside the clip" early return.
///
/// The far edges are summed in 64-bit, exactly as the C widened them, so a cell
/// pinned at the end of the coordinate space cannot wrap back into view.
/// `gw`/`gh` are the font's `uint8_t` geometry, so neither sum can underflow.
pub fn glyphWindow(x: i32, y: i32, gw: u8, gh: u8, clip: Box) ?Box {
    const x0 = if (x >= clip.x0) x else clip.x0;
    const y0 = if (y >= clip.y0) y else clip.y0;
    const xr = @as(i64, x) + @as(i64, gw);
    const yr = @as(i64, y) + @as(i64, gh);
    const x1: i32 = if (xr < @as(i64, clip.x1)) @intCast(xr) else clip.x1;
    const y1: i32 = if (yr < @as(i64, clip.y1)) @intCast(yr) else clip.y1;
    const box = Box{ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1 };
    return if (box.isEmpty()) null else box;
}

/// Measured extent of a string (`ra8_gfx_text_size`).
pub const TextExtent = struct {
    w: u32,
    h: u32,
};

/// Characters a text call walks: up to the NUL, capped at `max_chars`.
pub fn textLength(str: [*]const u8) u32 {
    var n: u32 = 0;
    while (n < max_chars) : (n += 1) {
        if (str[n] == 0) break;
    }
    return n;
}

/// `n` cells wide by one cell tall, in the C's `uint32_t` arithmetic.
pub fn textExtent(n: u32, glyph_width: u8, glyph_height: u8) TextExtent {
    return .{ .w = n *% @as(u32, glyph_width), .h = @as(u32, glyph_height) };
}

/// `ra8_gfx_text_out`'s guard order: the null pair is judged BEFORE the init
/// check, so a pre-init call with a null string answers `null_ptr` while the
/// same call with a real string and font answers `not_initialized`.
pub fn textOutStatus(has_str: bool, has_font: bool, initialized: bool) u16 {
    if (!has_str or !has_font) return err.null_ptr;
    if (!initialized) return err.not_initialized;
    return err.ok;
}

/// `ra8_gfx_text_size` judges only its four pointers. It never reads the
/// framebuffer binding, so it measures a string before any init.
pub fn textSizeStatus(has_str: bool, has_font: bool, has_w: bool, has_h: bool) u16 {
    if (!has_str or !has_font or !has_w or !has_h) return err.null_ptr;
    return err.ok;
}

// --- blue-noise dither (#477) ----------------------------------------------

/// The generated void-and-cluster threshold texture. Emitted by
/// `scripts/gen/gen_bluenoise_mask.py`; this is the only committed copy.
const dither_mask_table = @import("dither_mask.zig");

/// `ra8_gfx_dither_const_t` and `ra8_gfx_dither_scale_t` from
/// `inc/ra8_gfx_dither.h`.
pub const dither = struct {
    pub const levels: u8 = 16;
    pub const step: u8 = 17;
    pub const max_level: u8 = 15;
    pub const nib_shift: u5 = 4;
    pub const ppb: u32 = 2;
    pub const mask_dim: u32 = 64;
    pub const mask_index_mask: u32 = 63;
    pub const rgb_g_shift: u5 = 8;
    pub const rgb_r_shift: u5 = 16;
    pub const byte_levels: u32 = 256;
    pub const mask_len: u32 = 4096;
};

// The runtime index arithmetic assumes one `mask_dim` x `mask_dim` texture,
// reduced with a bitmask rather than a modulo. The C asserted both; so do we,
// so a regenerate that changed the geometry cannot silently mis-index.
comptime {
    std.debug.assert(dither_mask_table.mask.len == dither.mask_len);
    std.debug.assert(dither.mask_index_mask == dither.mask_dim - 1);
    std.debug.assert(@as(u32, dither.step) * @as(u32, dither.max_level) == 255);
}

/// Toroidal mask index for absolute panel coordinate (`x`, `y`).
///
/// AND with `dim - 1` is the mathematically-correct non-negative modulo for
/// negative coordinates too (two's complement), so a window drawn at a negative
/// offset still lands on the same continuous mask phase: seamless tiling.
pub fn maskIndex(x: i32, y: i32) u32 {
    const mx: u32 = @as(u32, @bitCast(x)) & dither.mask_index_mask;
    const my: u32 = @as(u32, @bitCast(y)) & dither.mask_index_mask;
    return (my * dither.mask_dim) + mx;
}

/// Blue-noise threshold for absolute panel coordinate (`x`, `y`).
pub fn maskThreshold(x: i32, y: i32) u8 {
    return dither_mask_table.mask[maskIndex(x, y)];
}

/// Quantise a gray8 sample to a 4-bit level given its blue-noise threshold.
///
/// The base level is `gray8 / step` and the fractional distance to the next
/// level is `(gray8 % step) / step`; the pixel rounds up when the threshold
/// falls below that fraction. Kept as the C's exact integer test
/// `thr * step < rem * byte_levels`, so the round-up probability is exactly
/// `rem / step` over a uniform mask: unbiased, no banding on a flat field.
/// No clamp is needed and none is added: the base equals `max_level` only when
/// `gray8 == 255`, which forces `rem == 0` and hence no round-up.
pub fn quantise(gray8: u8, thr: u8) u8 {
    const base: u8 = gray8 / dither.step;
    const rem: u8 = gray8 -% (base *% dither.step);
    const round_up = (@as(u32, thr) * @as(u32, dither.step)) <
        (@as(u32, rem) * dither.byte_levels);
    return if (round_up) base + 1 else base;
}

/// Quantise straight from the panel coordinate (`ra8_gfx_dither_gray4_level`).
pub fn ditherLevel(gray8: u8, x: i32, y: i32) u8 {
    return quantise(gray8, maskThreshold(x, y));
}

/// Expand a 4-bit panel level to a 0x00RRGGBB gray colour, byte-identical to
/// what `ra8_gfx_blit_gray8` and the gray4 zoom blit produce for that level.
pub fn levelToColor(level: u8) u32 {
    return grayToColor(gray4ToGray8(level));
}

/// Packed-gray4 byte count for a `w` x `h` tile, in the C's `uint32_t`
/// arithmetic: two pixels per byte, odd counts rounded up.
pub fn packedBytes(w: i32, h: i32) u32 {
    const n_pixels: u32 = @as(u32, @bitCast(w)) *% @as(u32, @bitCast(h));
    return (n_pixels +% 1) / dither.ppb;
}

/// Output byte holding packed pixel `i`.
pub fn packByteIndex(i: u32) u32 {
    return i / dither.ppb;
}

/// Even flat indices take the high nibble, odd ones the low nibble.
pub fn packIsHighNibble(i: u32) bool {
    return (i & 1) == 0;
}

/// Fold `level` into the output byte for flat index `i`. An even index ASSIGNS
/// the byte (clearing the low nibble) and an odd index ORs into it, so the
/// caller never has to pre-zero the buffer, not even for an odd pixel count.
pub fn packNibble(current: u8, level: u8, i: u32) u8 {
    if (packIsHighNibble(i)) return level << dither.nib_shift;
    return current | level;
}

/// `ra8_gfx_dither_gray8_to_gray4`'s guard order: the three pointers first,
/// each with its own log line, then the dimensions, then the capacity.
pub const PackGuard = enum { ok, no_src, no_out, no_out_size, bad_dims, too_small };

/// Judge one bulk-pack call. `out_cap` is only reached once the dimensions are
/// known good, so the byte count it is compared against is always well-formed.
pub fn packGuard(
    has_src: bool,
    has_out: bool,
    has_out_size: bool,
    w: i32,
    h: i32,
    out_cap: u32,
) PackGuard {
    if (!has_src) return .no_src;
    if (!has_out) return .no_out;
    if (!has_out_size) return .no_out_size;
    if ((w <= 0) or (h <= 0)) return .bad_dims;
    if (out_cap < packedBytes(w, h)) return .too_small;
    return .ok;
}

/// The `ra8_err_t` each bulk-pack verdict answers with.
pub fn packStatus(guard: PackGuard) u16 {
    return switch (guard) {
        .ok => err.ok,
        .no_src, .no_out, .no_out_size => err.null_ptr,
        .bad_dims => err.invalid_arg,
        .too_small => err.no_mem,
    };
}

/// `ra8_gfx_blit_gray8_dither`'s guard order, and it is the opposite of the
/// bulk packer's: the init check runs FIRST, so a pre-init call with a valid
/// buffer answers `not_initialized`. Neither guard logs on this entry point.
pub fn ditherBlitStatus(initialized: bool, has_src: bool, w: i32, h: i32) u16 {
    if (!initialized) return err.not_initialized;
    if (!has_src or (w <= 0) or (h <= 0)) return err.invalid_arg;
    return err.ok;
}
