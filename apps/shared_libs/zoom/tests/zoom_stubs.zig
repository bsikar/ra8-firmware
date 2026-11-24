// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Shared C-ABI test stubs and fixtures for `apps/shared_libs/zoom` test roots.

const std = @import("std");

/// Installs the silent `ra8_log_emit_error` backend sink.
pub fn LogSink() type {
    return struct {
        comptime {
            _ = &ra8_log_emit_error;
        }
        export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) callconv(.c) void {}
    };
}

/// Book descriptor and sub-rectangle read stubs for `zoom_book` tests.
pub fn BookStubs(comptime book: type) type {
    const Err = book.zoom.Err;
    const BookImage = book.BookImage;
    return struct {
        pub var test_img: BookImage = std.mem.zeroes(BookImage);
        pub var test_img_err: Err = .ok;
        pub var test_rect_calls: u32 = 0;
        pub var test_rect_err: Err = .ok;
        pub var test_last: struct { x: u32 = 0, y: u32 = 0, w: u32 = 0, h: u32 = 0, stride: u32 = 0 } = .{};

        comptime {
            _ = &book_src_image;
            _ = &book_src_image_rect;
        }

        export fn book_src_image(src: ?*const anyopaque, idx: u32, out_img: ?*BookImage) callconv(.c) Err {
            _ = src;
            if (out_img == null) return .null_ptr;
            out_img.?.* = test_img;
            if (idx == 0) {
                out_img.?.width = 0;
            }
            return test_img_err;
        }

        export fn book_src_image_rect(src: ?*const anyopaque, img: ?*const BookImage, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
            _ = src;
            _ = img;
            test_rect_calls += 1;
            test_last = .{ .x = x, .y = y, .w = w, .h = h, .stride = out_stride };
            if (out != null) out.?[0] = 0x80;
            return test_rect_err;
        }
    };
}

/// Dither and gray4-blit stubs for `zoom_render`, `zoom_popup`, and `zoom_abi` tests.
pub fn DitherBlitStubs(comptime render: type) type {
    const Err = render.zoom.Err;
    const ToneMap = render.ToneMap;
    return struct {
        pub const DitherArgs = struct { w: i32 = 0, h: i32 = 0, origin_x: i32 = 0, origin_y: i32 = 0, out_cap: u32 = 0 };
        pub const BlitArgs = struct { src_w: i32 = 0, src_h: i32 = 0, sw: i32 = 0, sh: i32 = 0, zoom: i32 = 0, dst_x: i32 = 0, dst_y: i32 = 0 };

        pub var dither_err: Err = .ok;
        pub var blit_err: Err = .ok;
        pub var dither_calls: u32 = 0;
        pub var blit_calls: u32 = 0;
        pub var dither_args: DitherArgs = .{};
        pub var blit_args: BlitArgs = .{};
        pub var read_fail: bool = false;

        comptime {
            _ = &ra8_gfx_dither_gray8_to_gray4_tone;
            _ = &ra8_gfx_blit_gray4_zoom;
        }

        export fn ra8_gfx_dither_gray8_to_gray4_tone(map: ?*const ToneMap, src: [*]const u8, w: i32, h: i32, origin_x: i32, origin_y: i32, out: [*]u8, out_cap: u32, out_size: *u32) callconv(.c) Err {
            _ = map;
            dither_calls += 1;
            dither_args = .{ .w = w, .h = h, .origin_x = origin_x, .origin_y = origin_y, .out_cap = out_cap };
            const n: usize = @intCast(w * h);
            var i: usize = 0;
            while (i < (n + 1) / 2) : (i += 1) {
                const hi: u8 = src[2 * i] >> 4;
                const lo: u8 = if (2 * i + 1 < n) src[2 * i + 1] >> 4 else 0;
                if (i < out_cap) {
                    out[i] = (hi << 4) | lo;
                }
            }
            out_size.* = @intCast((n + 1) / 2);
            return dither_err;
        }

        export fn ra8_gfx_blit_gray4_zoom(src: [*]const u8, src_w: i32, src_h: i32, sx: i32, sy: i32, sw: i32, sh: i32, zoom_arg: i32, dst_x: i32, dst_y: i32) callconv(.c) Err {
            _ = src;
            _ = sx;
            _ = sy;
            blit_calls += 1;
            blit_args = .{ .src_w = src_w, .src_h = src_h, .sw = sw, .sh = sh, .zoom = zoom_arg, .dst_x = dst_x, .dst_y = dst_y };
            return blit_err;
        }

        pub fn testReadFill(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
            if (read_fail) {
                return .no_mem;
            }
            const img: *const [64]u8 = @ptrCast(@alignCast(ctx));
            var r: u32 = 0;
            while (r < h) : (r += 1) {
                var c: u32 = 0;
                while (c < w) : (c += 1) {
                    out[r * out_stride + c] = img[(y + r) * 8 + (x + c)];
                }
            }
            return .ok;
        }
    };
}

/// Rect and face-text chrome draw stubs for `zoom_popup` and `zoom_abi` tests.
pub fn ChromeStubs(comptime Err: type) type {
    return struct {
        pub var rect_calls: u32 = 0;
        pub var text_calls: u32 = 0;
        pub var last_text: [48]u8 = @splat(0);

        comptime {
            _ = &ra8_gfx_rect;
            _ = &ra8_gfx_text_out_face;
        }

        export fn ra8_gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u32, filled: bool) callconv(.c) Err {
            _ = x;
            _ = y;
            _ = w;
            _ = h;
            _ = color;
            _ = filled;
            rect_calls += 1;
            return .ok;
        }

        export fn ra8_gfx_text_out_face(x: i32, y: i32, str: [*:0]const u8, face: u8, fg: u32, bg: u32) callconv(.c) Err {
            _ = x;
            _ = y;
            _ = face;
            _ = fg;
            _ = bg;
            text_calls += 1;
            const s = std.mem.span(str);
            const n = @min(s.len, last_text.len - 1);
            @memcpy(last_text[0..n], s[0..n]);
            last_text[n] = 0;
            return .ok;
        }

        pub fn stubRead(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
            _ = ctx;
            _ = x;
            _ = y;
            var r: u32 = 0;
            while (r < h) : (r += 1) {
                @memset(out[r * out_stride .. r * out_stride + w], 0x80);
            }
            return .ok;
        }
    };
}

/// Tile-cache acquisition, release, rect mapping, and prefetch stubs for `zoom_tiles` tests.
pub fn TileStubs(comptime tiles: type) type {
    const Err = tiles.zoom.Err;
    const TileCache = tiles.TileCache;
    const TileKey = tiles.TileKey;
    const Tile = tiles.Tile;
    const TileRect = tiles.TileRect;
    const TilePrefetchReq = tiles.TilePrefetchReq;
    const TileSrc = tiles.TileSrc;

    return struct {
        pub const t_cols: u16 = 4;
        pub const t_rows: u16 = 3;

        pub var get_err: Err = .ok;
        pub var put_err: Err = .ok;
        pub var rect_err: Err = .ok;
        pub var pf_err: Err = .ok;
        pub var get_calls: u32 = 0;
        pub var put_calls: u32 = 0;
        pub var mismatch: bool = false;
        pub var warmed: u16 = 0;
        pub var pf_req: TilePrefetchReq = .{};
        pub var mirror: struct { width: u32, height: u32, tile_w: u32, tile_h: u32 } = .{ .width = 100, .height = 90, .tile_w = 32, .tile_h = 32 };
        pub var cache_cell: u8 = 0;
        pub var tile_bytes: [t_cols * t_rows][32 * 32]u8 = undefined;

        comptime {
            _ = &ra8_tile_cache_get;
            _ = &ra8_tile_cache_put;
            _ = &ra8_tile_rect_of_pixels;
            _ = &ra8_tile_cache_prefetch_pan;
        }

        export fn ra8_tile_cache_get(tc: ?*TileCache, key: ?*const TileKey, out_tile: ?*Tile) callconv(.c) Err {
            _ = tc;
            get_calls += 1;
            if (out_tile == null) return .null_ptr;
            if (get_err != .ok) return get_err;
            const tx = key.?.tile_x;
            const ty = key.?.tile_y;
            const exp_w: u16 = @intCast(@min(mirror.tile_w, mirror.width - @as(u32, tx) * mirror.tile_w));
            const exp_h: u16 = @intCast(@min(mirror.tile_h, mirror.height - @as(u32, ty) * mirror.tile_h));
            const buf = &tile_bytes[@as(usize, ty) * t_cols + tx];
            for (buf, 0..) |*p, i| p.* = @truncate(0x40 + tx + ty * 4 + (i % 7));
            out_tile.?.pixels = buf[0..].ptr;
            out_tile.?.width = if (mismatch) exp_w - 1 else exp_w;
            out_tile.?.height = exp_h;
            return .ok;
        }

        export fn ra8_tile_cache_put(tc: ?*TileCache, pixels: ?[*]const u8) callconv(.c) Err {
            _ = tc;
            _ = pixels;
            put_calls += 1;
            return put_err;
        }

        export fn ra8_tile_rect_of_pixels(px: u32, py: u32, pw: u32, ph: u32, tile_w: u16, tile_h: u16, tile_cols: u16, tile_rows: u16, out: ?*TileRect) callconv(.c) Err {
            if (out == null) return .null_ptr;
            if (rect_err != .ok) return rect_err;
            if (pw == 0 or ph == 0) return .invalid_arg;
            out.?.tx0 = @intCast(@min(px / tile_w, tile_cols - 1));
            out.?.ty0 = @intCast(@min(py / tile_h, tile_rows - 1));
            out.?.tx1 = @intCast(@min((px + pw - 1) / tile_w, tile_cols - 1));
            out.?.ty1 = @intCast(@min((py + ph - 1) / tile_h, tile_rows - 1));
            return .ok;
        }

        export fn ra8_tile_cache_prefetch_pan(tc: ?*TileCache, req: ?*const TilePrefetchReq, out_warmed: ?*u16) callconv(.c) Err {
            _ = tc;
            if (req != null) pf_req = req.?.*;
            if (out_warmed) |w| w.* = warmed;
            return pf_err;
        }

        pub fn testCache() *TileCache {
            return @ptrCast(&cache_cell);
        }

        pub fn resetFixture() void {
            get_err = .ok;
            put_err = .ok;
            rect_err = .ok;
            pf_err = .ok;
            get_calls = 0;
            put_calls = 0;
            mismatch = false;
            warmed = 0;
            pf_req = .{};
            mirror = .{ .width = 100, .height = 90, .tile_w = 32, .tile_h = 32 };
        }

        pub fn boundFixture() TileSrc {
            var ts = std.mem.zeroes(TileSrc);
            const e = tiles.tileSrcInit(&ts, testCache(), 42, 100, 90, 32, 32);
            std.debug.assert(e == .ok);
            return ts;
        }

        pub fn expectedPixel(sx: usize, sy: usize) u8 {
            const tx = sx / 32;
            const ty = sy / 32;
            const tw: usize = @min(32, 100 - tx * 32);
            const th: usize = @min(32, 90 - ty * 32);
            const i = (sy - ty * 32) * tw + (sx - tx * 32);
            _ = th;
            return @truncate(0x40 + tx + ty * 4 + (i % 7));
        }
    };
}
