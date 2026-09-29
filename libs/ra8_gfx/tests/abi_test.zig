//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `ra8_gfx` C ABI membrane: the shared framebuffer
//! binding, the guard order and return codes of every exported entry point,
//! and the pixels each one actually leaves behind.

const std = @import("std");
const abi = @import("abi");
const impl = abi.internal;

/// A host framebuffer big enough for a 16x8 surface at any depth.
const Surface = struct {
    bytes: [16 * 8 * 4]u8 = [_]u8{0} ** (16 * 8 * 4),

    /// Publish this surface the way `ra8_gfx_init()`
    /// does: packed rows, full-surface clip. The bind TU stays C, so these
    /// tests write the shared object rather than calling through it.
    fn bind(self: *Surface, w: u16, h: u16, fmt: u8) void {
        self.bindPitch(w, h, fmt, @as(u32, w) * @as(u32, fmt));
    }

    /// The same, with an explicit row pitch, which is what
    /// `ra8_gfx_init_surface()` publishes for a padded buffer.
    fn bindPitch(self: *Surface, w: u16, h: u16, fmt: u8, pitch: u32) void {
        @memset(&self.bytes, 0);
        abi.g_gfx_text_state = .{
            .fb = &self.bytes,
            .width = w,
            .height = h,
            .pitch = pitch,
            .format = fmt,
            .bpp = impl.bppOf(fmt),
            .initialized = true,
            .clip_x0 = 0,
            .clip_y0 = 0,
            .clip_x1 = @intCast(w),
            .clip_y1 = @intCast(h),
        };
    }

    fn at(self: *Surface, x: usize, y: usize) u32 {
        const pitch: usize = abi.g_gfx_text_state.pitch;
        return impl.getPixel(&self.bytes, pitch, abi.g_gfx_text_state.format, x, y);
    }

    fn nonZeroCount(self: *Surface) usize {
        var n: usize = 0;
        const w: usize = abi.g_gfx_text_state.width;
        const h: usize = abi.g_gfx_text_state.height;
        var y: usize = 0;
        while (y < h) : (y += 1) {
            var x: usize = 0;
            while (x < w) : (x += 1) {
                if (self.at(x, y) != 0) n += 1;
            }
        }
        return n;
    }
};

/// Put the shared binding back to its pre-init value.
fn unbind() void {
    abi.g_gfx_text_state = .{ .format = impl.format.rgb565 };
}

/// C23 `bool` reaches the Zig ABI membrane as its one-byte integer value.
fn cBool(value: bool) u8 {
    return @intFromBool(value);
}

test "the shared binding starts unbound in RGB565" {
    unbind();
    try std.testing.expect(!abi.g_gfx_text_state.initialized);
    try std.testing.expectEqual(@as(u8, impl.format.rgb565), abi.g_gfx_text_state.format);
    try std.testing.expect(abi.g_gfx_text_state.fb == null);
}

test "every entry point refuses before init" {
    unbind();
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_clear(0));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_set_clip(0, 0, 1, 1));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_reset_clip());
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_pixel(0, 0, 0));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_line(0, 0, 1, 1, 0));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_rect(0, 0, 1, 1, 0, cBool(true)));
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_circle(0, 0, 1, 0, cBool(false)));
}

test "the promoted packer is the exported one" {
    try std.testing.expectEqual(@as(u16, 0xFFFF), abi.priv_gfx_text_pack_565(0x00FF_FFFF));
    try std.testing.expectEqual(@as(u16, 0), abi.priv_gfx_text_pack_565(0xFF00_0000));
}

test "the promoted plotter writes inside the clip and nowhere else" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(4, 2, 4, 2));

    abi.priv_gfx_text_plot(4, 2, 0x00FF_FFFF);
    abi.priv_gfx_text_plot(3, 2, 0x00FF_FFFF);
    abi.priv_gfx_text_plot(4, 1, 0x00FF_FFFF);
    abi.priv_gfx_text_plot(8, 2, 0x00FF_FFFF);
    abi.priv_gfx_text_plot(4, 4, 0x00FF_FFFF);

    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
    try std.testing.expect(s.at(4, 2) != 0);
}

test "clear fills the surface on the RGB565 fast path" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_clear(0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 16 * 8), s.nonZeroCount());
}

test "clear honours the clip on the fast path too" {
    // The RGB565 clear goes through the clipped rect fill, so a narrowed clip
    // narrows the clear: it is not a whole-surface wipe.
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(0, 0, 2, 2));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_clear(0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
    try std.testing.expect(s.at(0, 0) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(2, 0));
}

test "clear on the per-pixel path honours the clip" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(1, 1, 3, 2));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_clear(0x0011_2233));
    try std.testing.expectEqual(@as(usize, 6), s.nonZeroCount());
    try std.testing.expectEqual(@as(u32, 0x0011_2233), s.at(1, 1));
}

test "an interleaved RGB565 fill writes both halves of every word" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    // 0x18 >> 3 = 3 red, 0x40 >> 2 = 16 green, 0x80 >> 3 = 16 blue: halves differ.
    const color: u32 = 0x0018_4080;
    const word = impl.pack565(color);
    try std.testing.expect((word & 0xFF) != (word >> 8));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_clear(color));
    try std.testing.expectEqual(@as(usize, 16 * 8), s.nonZeroCount());
    try std.testing.expectEqual(impl.unpack565(word), s.at(3, 3));
}

test "set_clip clamps and reset_clip restores the full surface" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(-4, -4, 100, 100));
    try std.testing.expectEqual(@as(i32, 0), abi.g_gfx_text_state.clip_x0);
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x1);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(20, 20, 4, 4));
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x0);
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x1);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_reset_clip());
    try std.testing.expectEqual(@as(i32, 0), abi.g_gfx_text_state.clip_x0);
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x1);
    try std.testing.expectEqual(@as(i32, 8), abi.g_gfx_text_state.clip_y1);
}

test "pixel is bounded by the surface, not the clip" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.range_check_failed, abi.ra8_gfx_pixel(-1, 0, 0xFFFFFF));
    try std.testing.expectEqual(impl.err.range_check_failed, abi.ra8_gfx_pixel(0, -1, 0xFFFFFF));
    try std.testing.expectEqual(impl.err.range_check_failed, abi.ra8_gfx_pixel(16, 0, 0xFFFFFF));
    try std.testing.expectEqual(impl.err.range_check_failed, abi.ra8_gfx_pixel(0, 8, 0xFFFFFF));

    // Inside the surface but outside the clip: accepted, and silently dropped.
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(0, 0, 2, 2));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_pixel(9, 5, 0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_pixel(1, 1, 0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
}

test "a zero-extent rect is accepted and draws nothing" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(0, 0, 0, 4, 0xFFFFFF, cBool(true)));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(0, 0, 4, 0, 0xFFFFFF, cBool(true)));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(0, 0, -4, 4, 0xFFFFFF, cBool(false)));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "a filled rect covers exactly its own area on the fast path" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(2, 1, 4, 3, 0x00FF_FFFF, cBool(true)));
    try std.testing.expectEqual(@as(usize, 12), s.nonZeroCount());
    try std.testing.expect(s.at(2, 1) != 0);
    try std.testing.expect(s.at(5, 3) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(6, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(2, 4));
}

test "a filled rect on the per-pixel path covers the same area" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb888);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(2, 1, 4, 3, 0x0011_2233, cBool(true)));
    try std.testing.expectEqual(@as(usize, 12), s.nonZeroCount());
    try std.testing.expectEqual(@as(u32, 0x0011_2233), s.at(3, 2));
}

test "a filled rect is clipped to the clip box" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(4, 2, 2, 2));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(0, 0, 16, 8, 0x00FF_FFFF, cBool(true)));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "an outlined rect draws its border and leaves the interior alone" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(1, 1, 5, 4, 0x00FF_FFFF, cBool(false)));
    // Perimeter of a 5x4 box: 2*5 + 2*4 - 4 shared corners.
    try std.testing.expectEqual(@as(usize, 14), s.nonZeroCount());
    try std.testing.expect(s.at(1, 1) != 0);
    try std.testing.expect(s.at(5, 4) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(3, 2));
}

test "a one-pixel outlined rect is a single pixel" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_rect(7, 3, 1, 1, 0x00FF_FFFF, cBool(false)));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
    try std.testing.expect(s.at(7, 3) != 0);
}

test "a line walks from one endpoint to the other" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_line(0, 0, 5, 0, 0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 6), s.nonZeroCount());
    try std.testing.expect(s.at(0, 0) != 0);
    try std.testing.expect(s.at(5, 0) != 0);
}

test "a diagonal line touches both endpoints" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_line(1, 1, 6, 6, 0x00FF_FFFF));
    try std.testing.expect(s.at(1, 1) != 0);
    try std.testing.expect(s.at(6, 6) != 0);
    try std.testing.expectEqual(@as(usize, 6), s.nonZeroCount());
}

test "a degenerate line is one pixel and a fully clipped line is none" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_line(3, 3, 3, 3, 0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());

    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_line(-8, -8, -2, -2, 0x00FF_FFFF));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "circle refuses a negative radius" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_circle(8, 4, -1, 0xFFFFFF, cBool(false)));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "a zero-radius circle marks its centre only" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_circle(8, 4, 0, 0x00FF_FFFF, cBool(false)));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
    try std.testing.expect(s.at(8, 4) != 0);
}

test "an outlined circle stays on its radius" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_circle(8, 4, 3, 0x00FF_FFFF, cBool(false)));
    try std.testing.expect(s.at(11, 4) != 0);
    try std.testing.expect(s.at(5, 4) != 0);
    try std.testing.expect(s.at(8, 1) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 4));
}

test "a filled circle fills its centre and respects the clip" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_circle(8, 4, 3, 0x00FF_FFFF, cBool(true)));
    const filled = s.nonZeroCount();
    try std.testing.expect(s.at(8, 4) != 0);
    try std.testing.expect(filled > 20);

    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(8, 4, 1, 1));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_circle(8, 4, 3, 0x00FF_FFFF, cBool(true)));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
}

test "blit_gray8 rejects a null source and a non-positive extent" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit_gray8(null, 1, 1, 0, 0));
    const pixels = [_]u8{0xFF} ** 4;
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit_gray8(&pixels, 0, 1, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit_gray8(&pixels, 1, 0, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit_gray8(&pixels, -1, 1, 0, 0));
}

test "blit_gray8 lands gray levels on the RGB565 fast path" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const pixels = [_]u8{ 0xFF, 0x80, 0x40, 0x00 };
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8(&pixels, 2, 2, 3, 2));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(impl.grayToColor(0xFF))), s.at(3, 2));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(impl.grayToColor(0x80))), s.at(4, 2));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(impl.grayToColor(0x40))), s.at(3, 3));
    try std.testing.expectEqual(@as(u32, 0), s.at(4, 3));
}

test "blit_gray8 clips the fast path to the clip box" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(4, 2, 2, 2));
    const pixels = [_]u8{0xFF} ** (8 * 8);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8(&pixels, 8, 8, 0, 0));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "a fast-path blit fully outside the clip writes nothing" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const pixels = [_]u8{0xFF} ** 4;
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8(&pixels, 2, 2, 40, 40));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "blit_gray8 takes the per-pixel path for a non-RGB565 surface" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    const pixels = [_]u8{ 0x10, 0x20, 0x30, 0x40 };
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8(&pixels, 2, 2, 1, 1));
    try std.testing.expectEqual(impl.grayToColor(0x10), s.at(1, 1));
    try std.testing.expectEqual(impl.grayToColor(0x20), s.at(2, 1));
    try std.testing.expectEqual(impl.grayToColor(0x30), s.at(1, 2));
    try std.testing.expectEqual(impl.grayToColor(0x40), s.at(2, 2));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "blit judges null before init" {
    unbind();
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_blit(null, 1, 1, 2, 0, 0));
    const src = [_]u8{0} ** 8;
    try std.testing.expectEqual(impl.err.not_initialized, abi.ra8_gfx_blit(&src, 1, 1, 2, 0, 0));
}

test "blit rejects an empty source or an unknown source format" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const src = [_]u8{0} ** 8;
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit(&src, 0, 1, 2, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit(&src, 1, 0, 2, 0, 0));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_blit(&src, 1, 1, 1, 0, 0));
}

test "blit converts an RGB888 source onto an RGB565 surface" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const src = [_]u8{ 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00 };
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit(&src, 2, 1, impl.format.rgb888, 4, 2));
    try std.testing.expectEqual(@as(u32, 0x00F8_FCF8), s.at(4, 2));
    try std.testing.expectEqual(@as(u32, 0), s.at(5, 2));
}

test "blit copies an ARGB8888 source onto a matching surface" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    const src = [_]u8{ 0x33, 0x22, 0x11, 0x88 };
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit(&src, 1, 1, impl.format.argb8888, 2, 2),
    );
    try std.testing.expectEqual(@as(u32, 0x8811_2233), s.at(2, 2));
}

test "blit drops source pixels that fall outside the clip" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(0, 0, 1, 1));
    const src = [_]u8{0xFF} ** (4 * 4 * 2);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit(&src, 4, 4, impl.format.rgb565, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
}

test "the binding this archive defines is the one the C bind TU writes" {
    unbind();
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expect(abi.g_gfx_text_state.initialized);
    try std.testing.expectEqual(@as(u32, 32), abi.g_gfx_text_state.pitch);
    try std.testing.expectEqual(@as(u8, 2), abi.g_gfx_text_state.bpp);
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x1);
    try std.testing.expectEqual(@as(i32, 8), abi.g_gfx_text_state.clip_y1);
}

test "a padded surface addresses rows through the pitch, not width * bpp" {
    var s = Surface{};
    // Eight visible RGB565 pixels per row in a buffer whose rows are 32 bytes
    // wide: the 16 padding bytes at the end of each row must stay untouched.
    s.bindPitch(8, 4, impl.format.rgb565, 32);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_pixel(0, 1, 0x00FF_FFFF));

    try std.testing.expectEqual(@as(u8, 0xFF), s.bytes[32]);
    try std.testing.expectEqual(@as(u8, 0xFF), s.bytes[33]);
    var i: usize = 16;
    while (i < 32) : (i += 1) {
        try std.testing.expectEqual(@as(u8, 0), s.bytes[i]);
    }
}

test "a padded surface clears only its visible pixels" {
    var s = Surface{};
    s.bindPitch(8, 4, impl.format.rgb565, 32);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_clear(0x00FF_FFFF));

    var row: usize = 0;
    while (row < 4) : (row += 1) {
        var i: usize = 0;
        while (i < 16) : (i += 1) {
            try std.testing.expectEqual(@as(u8, 0xFF), s.bytes[(row * 32) + i]);
        }
        while (i < 32) : (i += 1) {
            try std.testing.expectEqual(@as(u8, 0), s.bytes[(row * 32) + i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Packed gray4 loupe zoom blit
// ---------------------------------------------------------------------------

/// Pack `levels` (one 0x0..0xF gray level per pixel, row-major) two per byte,
/// even flat index in the high nibble, which is the reader's native format.
fn packGray4(levels: []const u8, out: []u8) void {
    @memset(out, 0);
    for (levels, 0..) |level, flat| {
        if ((flat & 1) != 0) {
            out[flat >> 1] |= (level & 0x0F);
        } else {
            out[flat >> 1] |= (level << 4);
        }
    }
}

/// The RGB565 round trip of the gray a packed nibble expands to.
fn gray4Pixel(nibble: u8) u32 {
    const g8: u32 = @as(u32, (nibble << 4) | nibble);
    return impl.unpack565(impl.pack565(impl.grayToColor(g8)));
}

test "blit_gray4_zoom refuses before init" {
    unbind();
    const pixels = [_]u8{0xFF};
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 2, 1, 1, 0, 0),
    );
}

test "blit_gray4_zoom rejects a null source, a dead zoom and an empty image" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const pixels = [_]u8{0xFF};

    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(null, 2, 1, 0, 0, 2, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 2, 1, 0, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 2, 1, -1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 0, 1, 0, 0, 2, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 0, 0, 0, 2, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "blit_gray4_zoom checks init before the arguments" {
    unbind();
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_blit_gray4_zoom(null, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    );
}

test "blit_gray4_zoom at 1:1 reproduces every level as a gray pixel" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    const levels = [_]u8{ 0x0, 0x5, 0xA, 0xF };
    var pixels = [_]u8{0} ** 2;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 4, 1, 0, 0, 4, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(gray4Pixel(0x0), s.at(0, 0));
    try std.testing.expectEqual(gray4Pixel(0x5), s.at(1, 0));
    try std.testing.expectEqual(gray4Pixel(0xA), s.at(2, 0));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(3, 0));
    // Level 0 is black, so only three pixels are non-zero.
    try std.testing.expectEqual(@as(usize, 3), s.nonZeroCount());
}

test "blit_gray4_zoom magnifies one source pixel into a zoom-square block" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    const levels = [_]u8{ 0xF, 0x0 };
    var pixels = [_]u8{0} ** 1;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 1, 1, 3, 1, 2),
    );

    try std.testing.expectEqual(@as(usize, 9), s.nonZeroCount());
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(1, 2));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(3, 4));
    try std.testing.expectEqual(@as(u32, 0), s.at(0, 2));
    try std.testing.expectEqual(@as(u32, 0), s.at(4, 2));
    try std.testing.expectEqual(@as(u32, 0), s.at(1, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(1, 5));
}

test "blit_gray4_zoom places a sub-rectangle at dst plus offset times zoom" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    // 4x2 image, all white except the pixel at (2, 1), which is black.
    var levels = [_]u8{0xF} ** 8;
    levels[(1 * 4) + 2] = 0x0;
    var pixels = [_]u8{0} ** 4;
    packGray4(&levels, &pixels);

    // Sample the 2x1 window at (2, 1) at zoom 2, landing its first column at
    // dst_x + (2 - 2) * 2 == 0.
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 4, 2, 2, 1, 2, 1, 2, 0, 0),
    );

    try std.testing.expectEqual(@as(u32, 0), s.at(0, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(1, 1));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(2, 0));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(3, 1));
    // One 2x2 block of white, the other block black.
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "blit_gray4_zoom draws only the in-image part of an off-image window" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    const levels = [_]u8{ 0xF, 0xF, 0xF, 0xF };
    var pixels = [_]u8{0} ** 2;
    packGray4(&levels, &pixels);

    // Ask for four columns of a two-column image, starting one column left.
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 2, -1, 0, 4, 2, 1, 1, 0),
    );

    // Source column 0 keeps its natural dst + (0 - (-1)) * 1 == 2 position.
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(2, 0));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(3, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(1, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(4, 0));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "blit_gray4_zoom with a collapsed window draws nothing and still reports ok" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    const levels = [_]u8{ 0xF, 0xF };
    var pixels = [_]u8{0} ** 1;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 0, 1, 2, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 9, 9, 2, 2, 2, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "blit_gray4_zoom is clipped like every other draw" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(0, 0, 2, 2));

    const levels = [_]u8{0xF};
    var pixels = [_]u8{0} ** 1;
    packGray4(&levels, &pixels);

    // A 4x4 magnified block against a 2x2 clip leaves four pixels.
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 1, 1, 0, 0, 1, 1, 4, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(0, 0));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(1, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(2, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(0, 2));
}

test "blit_gray4_zoom honours the surface pitch on a padded framebuffer" {
    var s = Surface{};
    s.bindPitch(4, 4, impl.format.rgb565, 16 * 2);

    const levels = [_]u8{ 0xF, 0xF };
    var pixels = [_]u8{0} ** 1;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 2, 1, 2, 0, 0),
    );

    try std.testing.expectEqual(gray4Pixel(0xF), s.at(0, 0));
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(3, 1));
    // Row 1 begins a whole pitch in, so the bytes just past the visible
    // width of row 0 stay clear.
    try std.testing.expectEqual(@as(u8, 0), s.bytes[(4 * 2)]);
    try std.testing.expectEqual(@as(u8, 0), s.bytes[(4 * 2) + 1]);
}

test "blit_gray4_zoom takes the per-pixel path on a non-RGB565 surface" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);

    const levels = [_]u8{ 0x3, 0xC };
    var pixels = [_]u8{0} ** 1;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 2, 1, 0, 0, 2, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(impl.grayToColor(0x33), s.at(0, 0));
    try std.testing.expectEqual(impl.grayToColor(0xCC), s.at(1, 0));
}

test "blit_gray4_zoom reads the odd nibble of the last byte of an odd-width row" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    // 3x2 image: flat indices 0..5 span three bytes, and index 5 (the last
    // pixel) is the low nibble of the last one.
    const levels = [_]u8{ 0x1, 0x2, 0x3, 0x4, 0x5, 0xF };
    var pixels = [_]u8{0} ** 3;
    packGray4(&levels, &pixels);

    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&pixels, 3, 2, 2, 1, 1, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(gray4Pixel(0xF), s.at(0, 0));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
}

/// An 8x8 face whose slot 0 is solid, slot 1 blank, slot 2 a single top-left
/// pixel. Codepoints 'A'..'C' map onto those three slots.
const face_glyphs = [_]u8{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const face = impl.Font{
    .glyph_data = &face_glyphs,
    .glyph_width = 8,
    .glyph_height = 8,
    .bytes_per_glyph = 8,
    .first_codepoint = 'A',
    .last_codepoint = 'C',
};

test "text_out judges its pointers before the binding" {
    abi.g_gfx_text_state = .{};
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_out(0, 0, null, &face, 1, 0));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_out(0, 0, "A", null, 1, 0));
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_text_out(0, 0, "A", &face, 1, 0),
    );
}

test "text_size measures a string with no binding at all" {
    abi.g_gfx_text_state = .{};
    var w: u32 = 0;
    var h: u32 = 0;
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_size("ABC", &face, &w, &h));
    try std.testing.expectEqual(@as(u32, 24), w);
    try std.testing.expectEqual(@as(u32, 8), h);
}

test "text_size refuses any null argument" {
    var w: u32 = 0;
    var h: u32 = 0;
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size(null, &face, &w, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("A", null, &w, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("A", &face, null, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("A", &face, &w, null));
}

test "a solid glyph paints fg across its whole cell in RGB565" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "A", &face, 0xFFFFFF, 0));
    try std.testing.expectEqual(@as(usize, 64), s.nonZeroCount());
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(0, 0));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(7, 7));
}

test "a blank glyph paints bg across its whole cell" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "B", &face, 0xFFFFFF, 0xFF0000));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFF0000)), s.at(0, 0));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFF0000)), s.at(7, 7));
    // The cell is painted, the rest of the row is not.
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 0));
}

test "a codepoint outside the face renders as the first slot" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "z", &face, 0xFFFFFF, 0));
    // Slot 0 is the solid glyph, so the whole cell is foreground.
    try std.testing.expectEqual(@as(usize, 64), s.nonZeroCount());
}

test "each character advances the pen one cell width" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    // "BA": blank cell first, solid cell second.
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "BA", &face, 0xFFFFFF, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(0, 0));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(8, 0));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(15, 7));
}

test "the clip box confines a glyph cell" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(2, 2, 4, 4));

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "A", &face, 0xFFFFFF, 0));
    try std.testing.expectEqual(@as(usize, 16), s.nonZeroCount());
    try std.testing.expectEqual(@as(u32, 0), s.at(1, 1));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(2, 2));
    try std.testing.expectEqual(impl.unpack565(impl.pack565(0xFFFFFF)), s.at(5, 5));
}

test "a glyph fully outside the clip draws nothing" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(0, 0, 4, 4));

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(8, 0, "A", &face, 0xFFFFFF, 0));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "an empty string leaves the surface alone" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "", &face, 0xFFFFFF, 0xFF00));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "a glyph reaches a non-RGB565 surface through the shared plotter" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb888);

    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "C", &face, 0xFFFFFF, 0));
    // Slot 2 is one top-left pixel.
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
    try std.testing.expect(s.at(0, 0) != 0);
}

test "a face with no glyph table draws nothing and still succeeds" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);

    const empty = impl.Font{
        .glyph_data = null,
        .glyph_width = 8,
        .glyph_height = 8,
        .bytes_per_glyph = 8,
        .first_codepoint = 'A',
        .last_codepoint = 'C',
    };
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_out(0, 0, "A", &empty, 0xFFFFFF, 0));
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}
