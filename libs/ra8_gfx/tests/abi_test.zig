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

    fn bind(self: *Surface, w: u16, h: u16, fmt: u8) void {
        @memset(&self.bytes, 0);
        abi.g_gfx_text_state = .{ .format = impl.format.rgb565 };
        const status = abi.ra8_gfx_init(&self.bytes, w, h, fmt);
        std.debug.assert(status == impl.err.ok);
    }

    fn at(self: *Surface, x: usize, y: usize) u32 {
        const w: usize = abi.g_gfx_text_state.width;
        const bpp: usize = abi.g_gfx_text_state.bpp;
        return impl.getPixel(&self.bytes, w * bpp, abi.g_gfx_text_state.format, x, y);
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

test "init rejects a null framebuffer before anything else" {
    unbind();
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_init(null, 0, 0, 0));
    try std.testing.expect(!abi.g_gfx_text_state.initialized);
}

test "init rejects out-of-range dimensions and unknown formats" {
    var s = Surface{};
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 0, 8, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 8, 0, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 4097, 8, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 8, 4097, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 8, 8, 1));
}

test "init publishes the binding and a full-surface clip" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expect(abi.g_gfx_text_state.initialized);
    try std.testing.expectEqual(@as(u16, 16), abi.g_gfx_text_state.width);
    try std.testing.expectEqual(@as(u16, 8), abi.g_gfx_text_state.height);
    try std.testing.expectEqual(@as(u8, 2), abi.g_gfx_text_state.bpp);
    try std.testing.expectEqual(@as(i32, 0), abi.g_gfx_text_state.clip_x0);
    try std.testing.expectEqual(@as(i32, 16), abi.g_gfx_text_state.clip_x1);
    try std.testing.expectEqual(@as(i32, 8), abi.g_gfx_text_state.clip_y1);
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

test "re-init rebinds the surface and resets the clip" {
    var first = Surface{};
    var second = Surface{};
    first.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(2, 2, 2, 2));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_init(&second.bytes, 8, 4, impl.format.rgb888));
    try std.testing.expectEqual(@as(u16, 8), abi.g_gfx_text_state.width);
    try std.testing.expectEqual(@as(u8, 3), abi.g_gfx_text_state.bpp);
    try std.testing.expectEqual(@as(i32, 0), abi.g_gfx_text_state.clip_x0);
    try std.testing.expectEqual(@as(i32, 8), abi.g_gfx_text_state.clip_x1);
    try std.testing.expectEqual(@as(i32, 4), abi.g_gfx_text_state.clip_y1);
}

test "a failed re-init leaves the previous binding untouched" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_init(null, 4, 4, 2));
    try std.testing.expectEqual(impl.err.invalid_arg, abi.ra8_gfx_init(&s.bytes, 0, 4, 2));
    try std.testing.expectEqual(@as(u16, 16), abi.g_gfx_text_state.width);
    try std.testing.expect(abi.g_gfx_text_state.initialized);
}
