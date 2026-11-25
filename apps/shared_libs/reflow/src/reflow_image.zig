// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Zero-heap raster image decode + nearest-neighbour scale + blit
//! (src/reflow_image.c, RA8FW-1231).
//!
//! Zig port of the raster image surface declared in reflow_image.h:
//! probe dimensions and decode in-memory JPEG/PNG/GIF/BMP/WebP rasters
//! through the shared `ra8_imgdec_mux` backend matrix (`ra8_jpeg_imgdec_bind`
//! + `ra8_webp_imgdec_bind` + `ra8_img_imgdec_bind`), compute an
//! aspect-preserving fit rectangle inside the layout box, then
//! nearest-neighbour blit the scaled image into the ra8_gfx framebuffer,
//! one ra8_gfx_pixel() per destination pixel. All transient scratch and
//! decoded canvases stay within the caller-owned `ra8_img_arena` bump arena,
//! drained on every return path (NASA Rule 3).
//!
//! @since 0.1.0

const std = @import("std");
const imgdec = @import("ra8_img_imgdec.zig");

// ---- Error codes (libs/ra8_core/inc/ra8_err.h; ra8_err_t is a plain
// ---- c_int in the C ABI, so these mirror the C enumerators) ----

pub const k_ra8_ok: c_int = 0;
pub const k_ra8_err_no_mem: c_int = 0x102;
pub const k_ra8_err_invalid_arg: c_int = 0x103;
pub const k_ra8_err_invalid_size: c_int = 0x105;
pub const k_ra8_err_not_supported: c_int = 0x107;
pub const k_ra8_err_null_ptr: c_int = 0x504;

// ---- Pixel-packing and channel constants (no magic numbers) ----

/// Channel count for RGB888 surfaces.
const k_ra8_img_req_rgb: i32 = 3;
/// Channel count libwebp decodes into (RGBA8888).
const k_ra8_img_req_rgba: i32 = 4;
/// BT.601 integer luma weights (matches stbi__compute_y: (77*R + 150*G + 29*B) >> 8).
const k_ra8_luma_r: u32 = 77;
const k_ra8_luma_g: u32 = 150;
const k_ra8_luma_b: u32 = 29;
const k_ra8_luma_shift: u5 = 8;
/// Red byte offset within an RGB triple (same offsets in RGBA).
const k_ra8_img_ch_r: usize = 0;
/// Green byte offset within an RGB triple.
const k_ra8_img_ch_g: usize = 1;
/// Blue byte offset within an RGB triple.
const k_ra8_img_ch_b: usize = 2;
/// Minimum scaled / box edge length, pixels.
const k_ra8_img_min_edge: i32 = 1;
/// Red channel shift into 0x00RRGGBB.
const k_ra8_img_shift_r: u5 = 16;
/// Green channel shift into 0x00RRGGBB.
const k_ra8_img_shift_g: u5 = 8;
/// Mux capacity matching `k_ra8_imgdec_mux_max`.
const k_ra8_imgdec_mux_max: usize = 4;

// ---- Extern seam: shared decoder mux and platform sinks ----

extern fn ra8_img_imgdec_bind(out: *Ra8Imgdec, scratch: *Ra8ImgArena) callconv(.c) c_int;
extern fn ra8_jpeg_imgdec_bind(out: *Ra8Imgdec) callconv(.c) u16;
extern fn ra8_webp_imgdec_bind(out: *Ra8Imgdec, scratch: *Ra8ImgArena) callconv(.c) u16;

extern fn ra8_imgdec_mux_init(mux: *Ra8ImgdecMux) callconv(.c) u16;
extern fn ra8_imgdec_mux_add(mux: *Ra8ImgdecMux, dec: *const Ra8Imgdec) callconv(.c) u16;
extern fn ra8_imgdec_mux_decode(mux: *const Ra8ImgdecMux, req: *const Ra8ImgdecReq, out: *Ra8ImgdecImage) callconv(.c) u16;
extern fn ra8_imgdec_mux_probe(mux: *const Ra8ImgdecMux, bytes: [*]const u8, byte_count: u32, pixel: u32, out_geom: *Ra8ImgdecGeom, out_member: ?*?*const Ra8Imgdec) callconv(.c) u16;
extern fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32;

// SVG document surface (reflow_svg*.zig).
extern fn ra8_svg_is_svg(bytes: [*]const u8, len: usize) callconv(.c) bool;
extern fn ra8_svg_size(svg: [*]const u8, len: usize, out_w: *i32, out_h: *i32) callconv(.c) c_int;

// ra8_gfx framebuffer pixel hook (libs/ra8_gfx/inc/ra8_gfx.h).
extern fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) callconv(.c) c_int;

// ra8_log backend entry (ra8_log_error() expands to this in ra8_log.h).
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;

// ---- Shared ABI types (from ra8_img_imgdec.zig) ----

/// One spelling of ra8_imgdec_scratch_t: the ra8_webp_arena_t alias shares it.
pub const Ra8ImgArena = imgdec.Ra8ImgArena;
const Ra8Imgdec = imgdec.Ra8Imgdec;
const Ra8ImgdecReq = imgdec.Ra8ImgdecReq;
const Ra8ImgdecImage = imgdec.Ra8ImgdecImage;
const Ra8ImgdecGeom = imgdec.Ra8ImgdecGeom;

/// Mirror of `ra8_imgdec_mux_t` (libs/ra8_imgdec/inc/ra8_imgdec_mux.h).
const Ra8ImgdecMux = extern struct {
    members: [k_ra8_imgdec_mux_max]Ra8Imgdec,
    count: u32,
};

/// Log tag for the image decode/blit module.
const s_tag_img: [*:0]const u8 = "ra8_img";

fn logErr(message: [*:0]const u8) void {
    ra8_log_emit_error(s_tag_img, message);
}

/// Bind JPEG (first-party), WebP, and stb-residual (PNG/GIF/BMP) into `mux`.
fn initDecoderMux(mux: *Ra8ImgdecMux, webp_scratch: *Ra8ImgArena, stb_scratch: *Ra8ImgArena) c_int {
    var err = ra8_imgdec_mux_init(mux);
    if (err != 0) return @intCast(err);

    var dec: Ra8Imgdec = .{ .iface = null, .ctx = null };
    err = ra8_jpeg_imgdec_bind(&dec);
    if (err != 0) return @intCast(err);
    err = ra8_imgdec_mux_add(mux, &dec);
    if (err != 0) return @intCast(err);

    err = ra8_webp_imgdec_bind(&dec, webp_scratch);
    if (err != 0) return @intCast(err);
    err = ra8_imgdec_mux_add(mux, &dec);
    if (err != 0) return @intCast(err);

    const stb_err = ra8_img_imgdec_bind(&dec, stb_scratch);
    if (stb_err != k_ra8_ok) return stb_err;
    err = ra8_imgdec_mux_add(mux, &dec);
    return @intCast(err);
}

/// Probe `bytes` against each layout in priority order until a mux member matches.
fn probeForLayouts(
    mux: *const Ra8ImgdecMux,
    bytes: [*]const u8,
    byte_count: u32,
    layouts: []const u32,
    out_geom: *Ra8ImgdecGeom,
    out_want: *u32,
) c_int {
    for (layouts) |want| {
        const err = ra8_imgdec_mux_probe(mux, bytes, byte_count, want, out_geom, null);
        if (err == @as(u16, @intCast(k_ra8_ok))) {
            out_want.* = want;
            return k_ra8_ok;
        }
        if (err != @as(u16, @intCast(k_ra8_err_not_supported))) {
            return @intCast(err);
        }
    }
    return k_ra8_err_not_supported;
}

/// Map a mux/backend failure onto `reflow_image`'s published error set.
pub fn webpClassify(err: c_int) c_int {
    if (err == k_ra8_ok or err == k_ra8_err_null_ptr or err == k_ra8_err_invalid_arg) {
        return err;
    }
    return k_ra8_err_not_supported;
}

/// Map a decode failure preserving `k_ra8_err_no_mem` alongside argument errors.
fn classifyDecodeErr(err: c_int) c_int {
    if (err == k_ra8_err_no_mem) return k_ra8_err_no_mem;
    return webpClassify(err);
}

/// Compute the aspect-preserving fit rectangle for an image in a box.
///
/// Picks the largest integer scale that maps the source into the box
/// while preserving aspect ratio. The tighter axis is determined by
/// comparing `box_w * src_h` with `box_h * src_w` using i64 products to
/// prevent overflow on large dimensions. Both output dimensions are
/// clamped to at least k_ra8_img_min_edge (1 pixel) so downstream callers
/// never receive a zero-size rectangle.
pub fn fitBox(src_w: i32, src_h: i32, box_w: i32, box_h: i32, fit_w: *i32, fit_h: *i32) void {
    var sw: i64 = 0;
    var sh: i64 = 0;
    if (@as(i64, box_w) * @as(i64, src_h) <= @as(i64, box_h) * @as(i64, src_w)) {
        // Width is the tighter constraint: fill the box width.
        sw = @as(i64, box_w);
        sh = @divTrunc(@as(i64, src_h) * @as(i64, box_w), @as(i64, src_w));
    } else {
        // Height is the tighter constraint: fill the box height.
        sh = @as(i64, box_h);
        sw = @divTrunc(@as(i64, src_w) * @as(i64, box_h), @as(i64, src_h));
    }
    fit_w.* = @intCast(@max(sw, @as(i64, k_ra8_img_min_edge)));
    fit_h.* = @intCast(@max(sh, @as(i64, k_ra8_img_min_edge)));
}

/// One destination pixel write. The public path forwards to ra8_gfx_pixel
/// (which clips to the bound framebuffer); the unit tests record instead,
/// keeping the blit math testable without the framebuffer stack.
const PixelEmitter = *const fn (x: i32, y: i32, color: u32) void;

fn gfxEmit(x: i32, y: i32, color: u32) void {
    _ = ra8_gfx_pixel(x, y, color);
}

/// Nearest-neighbour blit a decoded image into the destination rectangle.
///
/// Iterates over every pixel in the fit_w x fit_h destination rectangle.
/// For each destination pixel (dx, dy) the corresponding source row and
/// column are computed with integer division scaled by i64 products to
/// avoid overflow: map_y = (dy * src_h) / fit_h and
/// map_x = (dx * src_w) / fit_w. The triple/quad at that position in the
/// row-major `pixels` buffer is packed into a 0x00RRGGBB word (the red,
/// green and blue bytes sit at the same offsets in the RGB and RGBA
/// packings, so only the stride differs) and emitted via `emit`.
pub fn blitScaled(pixels: [*]const u8, src_w: i32, src_h: i32, src_bpp: i32, fit_w: i32, fit_h: i32, dst_x: i32, dst_y: i32, emit: PixelEmitter) void {
    var dy: i32 = 0;
    while (dy < fit_h) : (dy += 1) {
        const map_y: usize = @intCast(@divTrunc(@as(i64, dy) * @as(i64, src_h), @as(i64, fit_h)));
        var dx: i32 = 0;
        while (dx < fit_w) : (dx += 1) {
            const map_x: usize = @intCast(@divTrunc(@as(i64, dx) * @as(i64, src_w), @as(i64, fit_w)));
            const idx: usize = (map_y * @as(usize, @intCast(src_w)) + map_x) * @as(usize, @intCast(src_bpp));
            const color: u32 = (@as(u32, pixels[idx + k_ra8_img_ch_r]) << k_ra8_img_shift_r) |
                (@as(u32, pixels[idx + k_ra8_img_ch_g]) << k_ra8_img_shift_g) |
                @as(u32, pixels[idx + k_ra8_img_ch_b]);
            emit(dst_x + dx, dst_y + dy, color);
        }
    }
}

/// Drain transient bookkeeping fields of `arena` while keeping `base`, `cap`,
/// and `high_water` intact.
fn arenaRelease(arena: *Ra8ImgArena) void {
    arena.offset = 0;
    arena.live = 0;
}

/// Convert `px_count` tightly packed RGB or RGBA pixels to 8-bit grayscale
/// using BT.601 integer luma (`(77*R + 150*G + 29*B) >> 8`).
fn packedToGray8(src: [*]const u8, px_count: usize, step: usize, dst: [*]u8) void {
    var i: usize = 0;
    while (i < px_count) : (i += 1) {
        const base = i * step;
        const r: u32 = src[base + k_ra8_img_ch_r];
        const g: u32 = src[base + k_ra8_img_ch_g];
        const b: u32 = src[base + k_ra8_img_ch_b];
        dst[i] = @intCast((r * k_ra8_luma_r + g * k_ra8_luma_g + b * k_ra8_luma_b) >> k_ra8_luma_shift);
    }
}

/// Convert `px_count` tightly packed RGBA8888 pixels to 8-bit grayscale.
///
/// Uses the exact BT.601 integer coefficients used by stb_image (`(77*R +
/// 150*G + 29*B) >> 8`), discarding the alpha channel so PNG and WebP gray8
/// decodes agree byte-for-byte.
pub fn rgbaToGray8(rgba: [*]const u8, px_count: usize, dst: [*]u8) void {
    packedToGray8(rgba, px_count, @intCast(k_ra8_img_req_rgba), dst);
}

/// Convert `px_count` tightly packed RGB888 pixels to 8-bit grayscale.
pub fn rgbToGray8(rgb: [*]const u8, px_count: usize, dst: [*]u8) void {
    packedToGray8(rgb, px_count, @intCast(k_ra8_img_req_rgb), dst);
}

/// Build an aligned WebP scratch view out of the unused suffix past `used`.
fn tailScratchView(arena: *const Ra8ImgArena, used: usize, out_tail_off: *usize) Ra8ImgArena {
    const base_ptr = arena.base orelse {
        out_tail_off.* = arena.cap;
        return .{ .base = null, .cap = 0, .offset = 0, .live = 0, .high_water = 0 };
    };
    const rem = arena.cap - used;
    const cur_addr = @intFromPtr(base_ptr) +% used;
    const pad: usize = (16 -% (cur_addr & 15)) & 15;
    if (pad >= rem) {
        out_tail_off.* = arena.cap;
        return .{ .base = null, .cap = 0, .offset = 0, .live = 0, .high_water = 0 };
    }
    const tail_off = used + pad;
    out_tail_off.* = tail_off;
    return .{
        .base = base_ptr + tail_off,
        .cap = arena.cap - tail_off,
        .offset = 0,
        .live = 0,
        .high_water = 0,
    };
}

/// Probe raster container geometry and select a supported destination layout.
fn probeRasterMux(
    bytes: [*]const u8,
    len: usize,
    layouts: []const u32,
    out_geom: *Ra8ImgdecGeom,
    out_want: *u32,
) c_int {
    if (len == 0 or len > @as(usize, std.math.maxInt(c_int))) {
        return k_ra8_err_not_supported;
    }
    var dummy_webp = Ra8ImgArena{ .base = null, .cap = 0, .offset = 0, .live = 0, .high_water = 0 };
    var dummy_stb = Ra8ImgArena{ .base = null, .cap = 0, .offset = 0, .live = 0, .high_water = 0 };
    var mux: Ra8ImgdecMux = undefined;
    const init_err = initDecoderMux(&mux, &dummy_webp, &dummy_stb);
    if (init_err != k_ra8_ok) {
        return webpClassify(init_err);
    }
    const err = probeForLayouts(&mux, bytes, @intCast(len), layouts, out_geom, out_want);
    if (err != k_ra8_ok) {
        return webpClassify(err);
    }
    return k_ra8_ok;
}

/// Carve `canvas_bytes` in the prefix of `arena` and decode via `ra8_imgdec_mux`.
fn decodeToArenaPrefix(
    arena: *Ra8ImgArena,
    bytes: [*]const u8,
    byte_count: u32,
    geom: Ra8ImgdecGeom,
    want: u32,
    canvas_bytes: usize,
    out_pixels: *[*]u8,
) c_int {
    const base_ptr = arena.base orelse {
        arenaRelease(arena);
        return k_ra8_err_no_mem;
    };
    if (canvas_bytes == 0 or canvas_bytes > arena.cap or canvas_bytes > @as(usize, std.math.maxInt(u32))) {
        arenaRelease(arena);
        return k_ra8_err_no_mem;
    }

    arena.offset = canvas_bytes;
    arena.live = 1;
    if (canvas_bytes > arena.high_water) {
        arena.high_water = canvas_bytes;
    }

    var webp_tail_off: usize = canvas_bytes;
    var webp_scratch = tailScratchView(arena, canvas_bytes, &webp_tail_off);
    var mux: Ra8ImgdecMux = undefined;
    const init_err = initDecoderMux(&mux, &webp_scratch, arena);
    if (init_err != k_ra8_ok) {
        arenaRelease(arena);
        return classifyDecodeErr(init_err);
    }

    const req = Ra8ImgdecReq{
        .bytes = bytes,
        .byte_count = byte_count,
        .arena = null,
        .dst = base_ptr,
        .dst_bytes = @intCast(canvas_bytes),
        .dst_stride = 0,
        .format = geom.format,
        .want = want,
    };
    var img: Ra8ImgdecImage = undefined;
    const dec_err = ra8_imgdec_mux_decode(&mux, &req, &img);
    const webp_peak = webp_tail_off +| webp_scratch.high_water;
    if (webp_peak > arena.high_water) {
        arena.high_water = webp_peak;
    }
    if (dec_err != 0) {
        arenaRelease(arena);
        logErr("img: mux decode failed");
        return classifyDecodeErr(@intCast(dec_err));
    }

    out_pixels.* = base_ptr;
    return k_ra8_ok;
}

/// Probe an image's intrinsic dimensions without a full decode.
///
/// Lets the layout pass size an `<img>` box from the source dimensions
/// before deciding to decode + blit. Allocates nothing. SVG is not a
/// raster: its intrinsic size comes from the document. Raster headers
/// route through `ra8_imgdec_mux_probe`.
pub export fn ra8_img_probe_size(bytes: ?[*]const u8, len: usize, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
    if (bytes == null) {
        logErr("probe: null bytes");
        return k_ra8_err_null_ptr;
    }
    if (out_w == null) {
        logErr("probe: null out_w");
        return k_ra8_err_null_ptr;
    }
    if (out_h == null) {
        logErr("probe: null out_h");
        return k_ra8_err_null_ptr;
    }
    const b = bytes.?;

    if (ra8_svg_is_svg(b, len)) {
        return ra8_svg_size(b, len, out_w.?, out_h.?);
    }

    const probe_layouts = [_]u32{ imgdec.k_ra8_imgdec_pixel_rgb888, imgdec.k_ra8_imgdec_pixel_rgba8888 };
    var geom: Ra8ImgdecGeom = undefined;
    var want: u32 = 0;
    const err = probeRasterMux(b, len, &probe_layouts, &geom, &want);
    if (err != k_ra8_ok) {
        logErr("probe: unsupported or invalid image");
        return err;
    }
    out_w.?.* = @intCast(geom.width_px);
    out_h.?.* = @intCast(geom.height_px);
    return k_ra8_ok;
}

/// Decode `bytes` and blit it, scaled to fit, into the bound framebuffer.
///
/// Routes JPEG/PNG/GIF/BMP/WebP through `ra8_imgdec_mux_decode` into a
/// caller-arena prefix buffer while backends draw transient scratch from
/// the aligned tail, computes a fit rectangle inside `box_w x box_h`, then
/// nearest-neighbour blits the scaled image via ra8_gfx. `arena` is reset
/// on every return path after argument validation.
pub export fn ra8_img_decode_blit(arena: ?*Ra8ImgArena, bytes: ?[*]const u8, len: usize, dst_x: i32, dst_y: i32, box_w: i32, box_h: i32, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
    if (arena == null) {
        logErr("blit: null arena");
        return k_ra8_err_null_ptr;
    }
    if (bytes == null) {
        logErr("blit: null bytes");
        return k_ra8_err_null_ptr;
    }
    if ((len == 0) or (box_w < k_ra8_img_min_edge) or (box_h < k_ra8_img_min_edge)) {
        logErr("blit: empty input or box");
        return k_ra8_err_invalid_arg;
    }
    const a = arena.?;
    const b = bytes.?;

    const blit_layouts = [_]u32{ imgdec.k_ra8_imgdec_pixel_rgb888, imgdec.k_ra8_imgdec_pixel_rgba8888 };
    var geom: Ra8ImgdecGeom = undefined;
    var want: u32 = 0;
    const probe_err = probeRasterMux(b, len, &blit_layouts, &geom, &want);
    if (probe_err != k_ra8_ok) {
        arenaRelease(a);
        return probe_err;
    }

    const bpp = ra8_imgdec_pixel_bytes(want);
    const px_count = std.math.mul(usize, geom.width_px, geom.height_px) catch {
        arenaRelease(a);
        return k_ra8_err_no_mem;
    };
    const canvas_bytes = std.math.mul(usize, px_count, bpp) catch {
        arenaRelease(a);
        return k_ra8_err_no_mem;
    };

    var pixels: [*]u8 = undefined;
    const dec_err = decodeToArenaPrefix(a, b, @intCast(len), geom, want, canvas_bytes, &pixels);
    if (dec_err != k_ra8_ok) {
        return dec_err;
    }

    var fit_w: i32 = 0;
    var fit_h: i32 = 0;
    fitBox(@intCast(geom.width_px), @intCast(geom.height_px), box_w, box_h, &fit_w, &fit_h);
    blitScaled(pixels, @intCast(geom.width_px), @intCast(geom.height_px), @intCast(bpp), fit_w, fit_h, dst_x, dst_y, gfxEmit);
    arenaRelease(a);

    if (out_w) |p| {
        p.* = fit_w;
    }
    if (out_h) |p| {
        p.* = fit_h;
    }
    return k_ra8_ok;
}

/// Decode raster `bytes` to tightly packed gray8 using caller-owned storage.
pub export fn ra8_img_decode_gray8(
    arena: ?*Ra8ImgArena,
    bytes: ?[*]const u8,
    len: usize,
    dst: ?[*]u8,
    dst_cap: usize,
    out_w: ?*u32,
    out_h: ?*u32,
) callconv(.c) c_int {
    if (arena == null or bytes == null or dst == null or out_w == null or out_h == null) {
        logErr("gray8: null argument");
        return k_ra8_err_null_ptr;
    }
    if (len == 0 or dst_cap == 0 or len > @as(usize, std.math.maxInt(c_int))) {
        logErr("gray8: empty or oversized input");
        return k_ra8_err_invalid_arg;
    }
    const a = arena.?;
    const b = bytes.?;
    const d = dst.?;
    if (ra8_svg_is_svg(b, len)) {
        logErr("gray8: svg is not raster");
        return k_ra8_err_not_supported;
    }

    const gray_layouts = [_]u32{
        imgdec.k_ra8_imgdec_pixel_grey8,
        imgdec.k_ra8_imgdec_pixel_rgb888,
        imgdec.k_ra8_imgdec_pixel_rgba8888,
    };
    var geom: Ra8ImgdecGeom = undefined;
    var want: u32 = 0;
    const probe_err = probeRasterMux(b, len, &gray_layouts, &geom, &want);
    if (probe_err != k_ra8_ok) {
        arenaRelease(a);
        return probe_err;
    }

    const px_count = std.math.mul(usize, geom.width_px, geom.height_px) catch {
        arenaRelease(a);
        return k_ra8_err_no_mem;
    };
    if (dst_cap < px_count) {
        arenaRelease(a);
        logErr("gray8: dst buffer too small");
        return k_ra8_err_no_mem;
    }

    if (want == imgdec.k_ra8_imgdec_pixel_grey8) {
        a.offset = 0;
        a.live = 0;
        var dummy_webp = Ra8ImgArena{ .base = null, .cap = 0, .offset = 0, .live = 0, .high_water = 0 };
        var mux: Ra8ImgdecMux = undefined;
        const init_err = initDecoderMux(&mux, &dummy_webp, a);
        if (init_err != k_ra8_ok) {
            arenaRelease(a);
            return classifyDecodeErr(init_err);
        }
        const req = Ra8ImgdecReq{
            .bytes = b,
            .byte_count = @intCast(len),
            .arena = null,
            .dst = d,
            .dst_bytes = @intCast(px_count),
            .dst_stride = 0,
            .format = geom.format,
            .want = want,
        };
        var img: Ra8ImgdecImage = undefined;
        const dec_err = ra8_imgdec_mux_decode(&mux, &req, &img);
        arenaRelease(a);
        if (dec_err != 0) {
            logErr("gray8: mux decode failed");
            return classifyDecodeErr(@intCast(dec_err));
        }
        out_w.?.* = img.width_px;
        out_h.?.* = img.height_px;
        return k_ra8_ok;
    }

    const bpp = ra8_imgdec_pixel_bytes(want);
    const canvas_bytes = std.math.mul(usize, px_count, bpp) catch {
        arenaRelease(a);
        return k_ra8_err_no_mem;
    };
    var pixels: [*]u8 = undefined;
    const dec_err = decodeToArenaPrefix(a, b, @intCast(len), geom, want, canvas_bytes, &pixels);
    if (dec_err != k_ra8_ok) {
        return dec_err;
    }
    packedToGray8(pixels, px_count, @intCast(bpp), d);
    arenaRelease(a);
    out_w.?.* = geom.width_px;
    out_h.?.* = geom.height_px;
    return k_ra8_ok;
}
