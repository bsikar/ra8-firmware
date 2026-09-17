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

/// 4x4 packed gray4 ramp: the pixel at flat index f carries level f, so its
/// expanded gray is `(f << 4) | f`. Mirrors `k_g4_ramp4x4` in the C suite.
const gray4_ramp_4x4 = [_]u8{ 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };

/// 3x2 packed gray4 image, pixels 0..5: an odd width, so the nibble parity
/// staggers across rows. Mirrors `k_g4_odd3x2` in the C suite.
const gray4_odd_3x2 = [_]u8{ 0x01, 0x23, 0x45 };

/// The colour a gray4 level lands as, after the surface round trip.
fn gray4Expected(level: u8) u32 {
    return impl.grayToColor(@as(u32, impl.gray4ToGray8(level)));
}

test "blit_gray4_zoom judges init, then the source, then zoom, then the extent" {
    unbind();
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0),
    );
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(null, 4, 4, 0, 0, 4, 4, 1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 4, 4, 0, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 0, 4, 0, 0, 4, 4, 1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 0, 0, 0, 4, 4, 1, 0, 0),
    );
}

test "a 1:1 gray4 blit reproduces every pixel of the ramp" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0),
    );
    var y: u8 = 0;
    while (y < 4) : (y += 1) {
        var x: u8 = 0;
        while (x < 4) : (x += 1) {
            const flat = (y * 4) + x;
            try std.testing.expectEqual(gray4Expected(flat), s.at(x, y));
        }
    }
}

test "a 2x gray4 blit replicates each source pixel into a 2x2 block" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 2, 2, 2, 0, 0),
    );
    const expected = [2][2]u8{ .{ 0x0, 0x1 }, .{ 0x4, 0x5 } };
    var sy: usize = 0;
    while (sy < 2) : (sy += 1) {
        var sx: usize = 0;
        while (sx < 2) : (sx += 1) {
            const want = gray4Expected(expected[sy][sx]);
            var dy: usize = 0;
            while (dy < 2) : (dy += 1) {
                var dx: usize = 0;
                while (dx < 2) : (dx += 1) {
                    try std.testing.expectEqual(want, s.at((sx * 2) + dx, (sy * 2) + dy));
                }
            }
        }
    }
}

test "an odd source width staggers the nibble parity across rows" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_odd_3x2, 3, 2, 0, 0, 3, 2, 1, 0, 0),
    );
    var y: u8 = 0;
    while (y < 2) : (y += 1) {
        var x: u8 = 0;
        while (x < 3) : (x += 1) {
            try std.testing.expectEqual(gray4Expected((y * 3) + x), s.at(x, y));
        }
    }
}

test "a sub-rectangle running off the image edge draws only its in-image part" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 2, 0, 4, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(gray4Expected(2), s.at(0, 0));
    try std.testing.expectEqual(gray4Expected(3), s.at(1, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(2, 0));
    // Only source columns 2 and 3 exist, so exactly two pixels carry ink.
    try std.testing.expectEqual(@as(usize, 2), s.nonZeroCount());
}

test "a gray4 blit running off the surface keeps only the visible columns" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 4, 1, 1, 14, 0),
    );
    try std.testing.expectEqual(@as(u32, 0), s.at(14, 0));
    try std.testing.expectEqual(gray4Expected(1), s.at(15, 0));
    try std.testing.expectEqual(@as(usize, 1), s.nonZeroCount());
}

test "a gray4 blit is clipped like every other draw" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(1, 1, 2, 2));
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0),
    );
    try std.testing.expectEqual(gray4Expected(5), s.at(1, 1));
    try std.testing.expectEqual(gray4Expected(6), s.at(2, 1));
    try std.testing.expectEqual(gray4Expected(9), s.at(1, 2));
    try std.testing.expectEqual(gray4Expected(10), s.at(2, 2));
    try std.testing.expectEqual(@as(usize, 4), s.nonZeroCount());
}

test "a collapsed gray4 sub-rectangle is accepted and draws nothing" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 0, 0, 0, 0, 1, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 9, 9, 2, 2, 1, 0, 0),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "a gray4 blit reaches the RGB565 surface through the shared plotter" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_blit_gray4_zoom(&gray4_ramp_4x4, 4, 4, 3, 3, 1, 1, 1, 0, 0),
    );
    try std.testing.expectEqual(
        impl.unpack565(impl.pack565(gray4Expected(15))),
        s.at(0, 0),
    );
}

/// A two-slot 8x2 test face: slot 0 ('a') is blank, slot 1 ('b') is solid.
const face_glyphs = [_]u8{ 0x00, 0x00, 0xFF, 0xFF };
const test_face = impl.Font{
    .glyph_data = &face_glyphs,
    .glyph_width = 8,
    .glyph_height = 2,
    .bytes_per_glyph = 2,
    .first_codepoint = 'a',
    .last_codepoint = 'b',
};

const text_fg: u32 = 0x00FF0000;
const text_bg: u32 = 0x000000FF;

/// The colour a 565 surface actually holds after storing `color`.
fn stored565(color: u32) u32 {
    return impl.unpack565(impl.pack565(color));
}

test "text_out judges its pointers before the binding" {
    unbind();
    try std.testing.expectEqual(
        impl.err.null_ptr,
        abi.ra8_gfx_text_out(0, 0, null, &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(
        impl.err.null_ptr,
        abi.ra8_gfx_text_out(0, 0, "b", null, text_fg, text_bg),
    );
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_text_out(0, 0, "b", &test_face, text_fg, text_bg),
    );
}

test "text_size measures a string with no binding at all" {
    unbind();
    var w: u32 = 0;
    var h: u32 = 0;
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_size("abc", &test_face, &w, &h));
    try std.testing.expectEqual(@as(u32, 24), w);
    try std.testing.expectEqual(@as(u32, 2), h);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_text_size("", &test_face, &w, &h));
    try std.testing.expectEqual(@as(u32, 0), w);
    try std.testing.expectEqual(@as(u32, 2), h);
}

test "text_size refuses any null argument" {
    var w: u32 = 0;
    var h: u32 = 0;
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size(null, &test_face, &w, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("a", null, &w, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("a", &test_face, null, &h));
    try std.testing.expectEqual(impl.err.null_ptr, abi.ra8_gfx_text_size("a", &test_face, &w, null));
}

test "a solid glyph paints fg across its whole cell in RGB565" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "b", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(stored565(text_fg), s.at(0, 0));
    try std.testing.expectEqual(stored565(text_fg), s.at(7, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 0));
    try std.testing.expectEqual(@as(u32, 0), s.at(0, 2));
}

test "a blank glyph paints bg across its whole cell" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "a", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(stored565(text_bg), s.at(0, 0));
    try std.testing.expectEqual(stored565(text_bg), s.at(7, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 0));
}

test "a codepoint outside the face renders as the first slot" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "z", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(stored565(text_bg), s.at(0, 0));
    try std.testing.expectEqual(stored565(text_bg), s.at(7, 1));
}

test "each character advances the pen one cell width" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "ab", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(stored565(text_bg), s.at(0, 0));
    try std.testing.expectEqual(stored565(text_fg), s.at(8, 0));
    try std.testing.expectEqual(stored565(text_fg), s.at(15, 1));
}

test "the clip box confines a glyph cell" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(4, 0, 4, 8));
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "b", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(@as(u32, 0), s.at(3, 0));
    try std.testing.expectEqual(stored565(text_fg), s.at(4, 0));
    try std.testing.expectEqual(stored565(text_fg), s.at(7, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 0));
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_reset_clip());
}

test "a glyph fully outside the clip draws nothing" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_set_clip(12, 0, 4, 8));
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "b", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_reset_clip());
}

test "an empty string leaves the surface alone" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

test "a glyph reaches a non-RGB565 surface through the shared plotter" {
    var s = Surface{};
    s.bind(16, 8, impl.format.argb8888);
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "b", &test_face, text_fg, text_bg),
    );
    try std.testing.expectEqual(text_fg, s.at(0, 0));
    try std.testing.expectEqual(text_fg, s.at(7, 1));
    try std.testing.expectEqual(@as(u32, 0), s.at(8, 0));
}

test "a face with no glyph table draws nothing and still succeeds" {
    var s = Surface{};
    s.bind(16, 8, impl.format.rgb565);
    const bare = impl.Font{
        .glyph_width = 8,
        .glyph_height = 2,
        .bytes_per_glyph = 2,
        .first_codepoint = 'a',
        .last_codepoint = 'b',
    };
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_text_out(0, 0, "b", &bare, text_fg, text_bg),
    );
    try std.testing.expectEqual(@as(usize, 0), s.nonZeroCount());
}

// --- blue-noise dither (#477) ----------------------------------------------

/// The bulk packer's guards each carry their own message, so the test binary
/// stands in for `ra8_core`'s logger the same way CMake links the real one.
var dither_log_count: u32 = 0;
var dither_log_tag: [*:0]const u8 = "";
var dither_log_message: [*:0]const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    dither_log_count += 1;
    dither_log_tag = tag;
    dither_log_message = message;
}

fn resetDitherLog() void {
    dither_log_count = 0;
    dither_log_tag = "";
    dither_log_message = "";
}

fn expectDitherLog(count: u32, message: []const u8) !void {
    try std.testing.expectEqual(count, dither_log_count);
    try std.testing.expectEqualStrings("ra8_gfx_dither", std.mem.span(dither_log_tag));
    try std.testing.expectEqualStrings(message, std.mem.span(dither_log_message));
}

test "ra8_gfx_dither_gray4_level needs no framebuffer binding" {
    abi.g_gfx_text_state = .{ .format = impl.format.rgb565 };
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_gfx_dither_gray4_level(0, 3, 4));
    try std.testing.expectEqual(@as(u8, 15), abi.ra8_gfx_dither_gray4_level(255, 3, 4));
    try std.testing.expectEqual(
        impl.ditherLevel(200, 3, 4),
        abi.ra8_gfx_dither_gray4_level(200, 3, 4),
    );
}

test "the mask phase repeats every 64 pixels in both axes" {
    try std.testing.expectEqual(
        abi.ra8_gfx_dither_gray4_level(120, 2, 5),
        abi.ra8_gfx_dither_gray4_level(120, 66, 69),
    );
    try std.testing.expectEqual(
        abi.ra8_gfx_dither_gray4_level(120, 2, 5),
        abi.ra8_gfx_dither_gray4_level(120, -62, -59),
    );
}

test "the bulk packer rejects each null pointer with its own message" {
    const src = [_]u8{0} ** 16;
    var out: [8]u8 = undefined;
    var out_size: u32 = 0xFFFF_FFFF;

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.null_ptr,
        abi.ra8_gfx_dither_gray8_to_gray4(null, 4, 4, 0, 0, &out, out.len, &out_size),
    );
    try expectDitherLog(1, "src must not be nullptr");

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.null_ptr,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 4, 4, 0, 0, null, out.len, &out_size),
    );
    try expectDitherLog(1, "out must not be nullptr");

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.null_ptr,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 4, 4, 0, 0, &out, out.len, null),
    );
    try expectDitherLog(1, "out_size must not be nullptr");
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), out_size);
}

test "non-positive dimensions and a short buffer are distinct failures" {
    const src = [_]u8{128} ** 16;
    var out: [8]u8 = [_]u8{0} ** 8;
    var out_size: u32 = 0;

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 0, 4, 0, 0, &out, out.len, &out_size),
    );
    try expectDitherLog(1, "w or h is non-positive");

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 4, -1, 0, 0, &out, out.len, &out_size),
    );
    try expectDitherLog(1, "w or h is non-positive");

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.no_mem,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 4, 4, 0, 0, &out, 7, &out_size),
    );
    try expectDitherLog(1, "output buffer too small");
    try std.testing.expectEqual(@as(u32, 0), out_size);
}

test "a packed tile carries two levels per byte and reports its size" {
    var src: [16]u8 = undefined;
    for (&src, 0..) |*p, i| p.* = @intCast(i * 17);
    var out: [8]u8 = [_]u8{0xFF} ** 8;
    var out_size: u32 = 0;

    resetDitherLog();
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 4, 4, 0, 0, &out, out.len, &out_size),
    );
    try std.testing.expectEqual(@as(u32, 8), out_size);
    try std.testing.expectEqual(@as(u32, 0), dither_log_count);

    for (0..16) |i| {
        const want = impl.ditherLevel(src[i], @intCast(i % 4), @intCast(i / 4));
        const byte = out[i / 2];
        const got: u8 = if (i % 2 == 0) byte >> 4 else byte & 0x0F;
        try std.testing.expectEqual(want, got);
    }
}

test "an odd pixel count still needs no pre-zeroed buffer" {
    const src = [_]u8{ 9, 200, 77 };
    var out: [2]u8 = [_]u8{ 0xFF, 0xFF };
    var out_size: u32 = 0;
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_dither_gray8_to_gray4(&src, 3, 1, 0, 0, &out, out.len, &out_size),
    );
    try std.testing.expectEqual(@as(u32, 2), out_size);
    try std.testing.expectEqual(impl.ditherLevel(9, 0, 0), out[0] >> 4);
    try std.testing.expectEqual(impl.ditherLevel(200, 1, 0), out[0] & 0x0F);
    try std.testing.expectEqual(impl.ditherLevel(77, 2, 0), out[1] >> 4);
    // The trailing low nibble is whatever the even-index assign left: zero.
    try std.testing.expectEqual(@as(u8, 0), out[1] & 0x0F);
}

test "a tile packs byte-for-byte the same as that region of the whole image" {
    var whole: [8 * 4]u8 = undefined;
    for (&whole, 0..) |*p, i| p.* = @intCast((i * 7) & 0xFF);
    var whole_out: [16]u8 = undefined;
    var whole_size: u32 = 0;
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_dither_gray8_to_gray4(&whole, 8, 4, 0, 0, &whole_out, whole_out.len, &whole_size),
    );

    var tile: [4 * 4]u8 = undefined;
    for (0..4) |row| {
        for (0..4) |col| tile[(row * 4) + col] = whole[(row * 8) + col + 4];
    }
    var tile_out: [8]u8 = undefined;
    var tile_size: u32 = 0;
    try std.testing.expectEqual(
        impl.err.ok,
        abi.ra8_gfx_dither_gray8_to_gray4(&tile, 4, 4, 4, 0, &tile_out, tile_out.len, &tile_size),
    );
    try std.testing.expectEqual(@as(u32, 8), tile_size);

    for (0..16) |i| {
        const whole_i = ((i / 4) * 8) + (i % 4) + 4;
        const from_whole: u8 = if (whole_i % 2 == 0)
            whole_out[whole_i / 2] >> 4
        else
            whole_out[whole_i / 2] & 0x0F;
        const from_tile: u8 = if (i % 2 == 0) tile_out[i / 2] >> 4 else tile_out[i / 2] & 0x0F;
        try std.testing.expectEqual(from_whole, from_tile);
    }
}

test "the dithered blit refuses a call with no binding" {
    abi.g_gfx_text_state = .{ .format = impl.format.rgb565 };
    const src = [_]u8{128} ** 4;
    try std.testing.expectEqual(
        impl.err.not_initialized,
        abi.ra8_gfx_blit_gray8_dither(&src, 2, 2, 0, 0),
    );
}

test "the dithered blit rejects a null tile and non-positive dimensions" {
    var s = Surface{};
    s.bind(8, 4, impl.format.rgb565);
    const src = [_]u8{128} ** 16;
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray8_dither(null, 4, 4, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray8_dither(&src, 0, 4, 0, 0),
    );
    try std.testing.expectEqual(
        impl.err.invalid_arg,
        abi.ra8_gfx_blit_gray8_dither(&src, 4, -2, 0, 0),
    );
}

test "the dithered blit lays down exactly the per-level plot" {
    var expected = Surface{};
    expected.bind(8, 4, impl.format.rgb565);
    var src: [8 * 4]u8 = undefined;
    for (&src, 0..) |*p, i| p.* = @intCast((i * 11) & 0xFF);
    for (0..4) |row| {
        for (0..8) |col| {
            const level = impl.ditherLevel(src[(row * 8) + col], @intCast(col), @intCast(row));
            try std.testing.expectEqual(
                impl.err.ok,
                abi.ra8_gfx_pixel(@intCast(col), @intCast(row), impl.levelToColor(level)),
            );
        }
    }

    var got = Surface{};
    got.bind(8, 4, impl.format.rgb565);
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8_dither(&src, 8, 4, 0, 0));
    try std.testing.expectEqualSlices(u8, &expected.bytes, &got.bytes);
}

test "the dithered blit draws only the part that lands on the surface" {
    var s = Surface{};
    s.bind(8, 4, impl.format.rgb565);
    const src = [_]u8{200} ** 16;
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8_dither(&src, 4, 4, 6, 2));
    try std.testing.expect(s.at(6, 2) != 0);
    try std.testing.expect(s.at(7, 3) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(5, 2));
    try std.testing.expectEqual(@as(u32, 0), s.at(0, 0));
}

test "a negative destination clips and keeps the mask phase continuous" {
    var s = Surface{};
    s.bind(8, 4, impl.format.rgb565);
    const src = [_]u8{140} ** 16;
    try std.testing.expectEqual(impl.err.ok, abi.ra8_gfx_blit_gray8_dither(&src, 4, 4, -2, -1));
    try std.testing.expect(s.at(0, 0) != 0);
    try std.testing.expectEqual(@as(u32, 0), s.at(2, 0));
}
