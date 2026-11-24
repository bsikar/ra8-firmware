//! Zero-heap raster image decode + nearest-neighbour scale + blit
//! (src/reflow_image.c, RA8FW-1231).
//!
//! Zig port of the raster image surface declared in reflow_image.h:
//! decode an in-memory JPEG/PNG/GIF/BMP through the vendored stb_image,
//! compute an aspect-preserving fit rectangle inside the layout box, then
//! nearest-neighbour blit the scaled image into the ra8_gfx framebuffer,
//! one ra8_gfx_pixel() per destination pixel. The decode reaches no
//! malloc: stb_image's allocator is redirected to the caller-owned
//! ra8_img_arena bump arena, and the arena is fully drained on every
//! return path.
//!
//! stb_image cannot decode WebP, so a RIFF/WEBP buffer is dispatched to
//! the ra8_webp facade over the vendored libwebp: the inline small-image
//! path takes the same formats the band-tile producer does, and an EPUB
//! whose illustrations are WebP renders them instead of dropping them.
//! The WebP canvas is RGBA8888, so the blit carries a source
//! bytes-per-pixel rather than assuming the stb RGB triple.
//!
//! The whole decode stack (stb_image, the ra8_webp facade, the shared
//! ra8_imgdec container sniff, the arena allocator, the SVG document
//! surface, ra8_gfx and ra8_log) stays behind the extern seam; the
//! exports keep C linkage so the C callers (layout, render, apps,
//! test suites) link against them exactly as before.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");

// ---- Error codes (libs/ra8_core/inc/ra8_err.h; ra8_err_t is a plain
// ---- c_int in the C ABI, so these mirror the C enumerators) ----

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;
const k_ra8_err_invalid_arg: c_int = 0x103;
const k_ra8_err_not_supported: c_int = 0x107;
const k_ra8_err_null_ptr: c_int = 0x504;

// ---- Pixel-packing and channel constants (no magic numbers) ----

/// Desired channel count requested from stb_image.
const k_ra8_img_req_rgb: c_int = 3;
/// Single-channel gray8 output requested from stb_image.
const k_ra8_img_req_gray: c_int = 1;
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

// ---- Shared container sniff (libs/ra8_imgdec/inc/ra8_imgdec.h) ----

/// Leading bytes ra8_imgdec_sniff() reads; longer buffers are clamped to it.
const k_ra8_imgdec_sniff_bytes: u32 = 12;
/// The WebP format bit (k_ra8_imgdec_format_webp, 1 << 2).
const k_ra8_imgdec_format_webp: u32 = 1 << 2;

// ---- Extern seam: the decode stack stays C ----

// stb_image (apps/shared_libs/third_party/stb/stb_image.h).
extern fn stbi_info_from_memory(buffer: [*]const u8, len: c_int, x: *c_int, y: *c_int, comp: *c_int) callconv(.c) c_int;
extern fn stbi_load_from_memory(buffer: [*]const u8, len: c_int, x: *c_int, y: *c_int, channels_in_file: *c_int, desired_channels: c_int) callconv(.c) ?[*]u8;
extern fn stbi_failure_reason() callconv(.c) ?[*:0]const u8;
extern fn stbi_image_free(retval_from_stbi_load: *anyopaque) callconv(.c) void;

// Shared container sniff: the one RIFF/WEBP test in the tree (RA8FW-308).
extern fn ra8_imgdec_sniff(bytes: [*]const u8, byte_count: u32, out: *u32) callconv(.c) c_int;

// ra8_webp facade (apps/shared_libs/webp/inc/ra8_webp.h).
extern fn ra8_webp_get_info(data: [*]const u8, size: usize, out_w: *u32, out_h: *u32) callconv(.c) c_int;
extern fn ra8_webp_decode_rgba(data: [*]const u8, size: usize, arena: *Ra8ImgArena, out_rgba: [*]u8, out_stride: usize, out_capacity: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) c_int;

// ra8_img_arena bump allocator (apps/shared_libs/reflow/inc/ra8_img_arena.h).
extern fn ra8_img_arena_bind(arena: *Ra8ImgArena) callconv(.c) void;
extern fn ra8_img_arena_unbind() callconv(.c) void;
extern fn ra8_img_arena_malloc(n: usize) callconv(.c) ?*anyopaque;

// SVG document surface (reflow_svg*.c).
extern fn ra8_svg_is_svg(bytes: [*]const u8, len: usize) callconv(.c) bool;
extern fn ra8_svg_size(svg: [*]const u8, len: usize, out_w: *i32, out_h: *i32) callconv(.c) c_int;

// ra8_gfx framebuffer pixel hook (libs/ra8_gfx/inc/ra8_gfx.h).
extern fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) callconv(.c) c_int;

// ra8_log backend entry (ra8_log_error() expands to this in ra8_log.h).
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;

// ---- Caller-owned bump arena (ra8_imgdec_scratch_t, RA8FW-308) ----

/// One spelling of ra8_imgdec_scratch_t: the ra8_webp_arena_t alias shares it.
pub const Ra8ImgArena = extern struct {
    base: ?[*]u8,
    cap: usize,
    offset: usize,
    live: usize,
    high_water: usize,
};

/// Log tag for the image decode/blit module.
const s_tag_img: [*:0]const u8 = "ra8_img";

fn logErr(message: [*:0]const u8) void {
    ra8_log_emit_error(s_tag_img, message);
}

/// Report whether `bytes` opens with a WebP container signature.
///
/// Forwards to ra8_imgdec_sniff(), the one container sniff in the tree
/// (RA8FW-308). The predicate stays deliberately cheap and keeps its old
/// meaning: it only decides which decoder sees the bytes, and the
/// ra8_webp facade does the real container and dimension validation.
/// Anything the sniff does not recognise as WebP -- including a buffer
/// too short to carry the signature, and a RIFF container whose form type
/// is something else -- falls through to stb_image and is rejected there,
/// rather than being mis-routed into the WebP facade.
fn isWebp(bytes: [*]const u8, len: usize) bool {
    const window: u32 = @intCast(@min(len, @as(usize, k_ra8_imgdec_sniff_bytes)));
    var fmt: u32 = 0;
    if (ra8_imgdec_sniff(bytes, window, &fmt) != k_ra8_ok) {
        return false;
    }
    return fmt == k_ra8_imgdec_format_webp;
}

/// Map an ra8_webp facade status onto ra8_img's documented error set.
///
/// The facade reports a richer set than this module publishes: a malformed
/// container, a non-positive canvas, and a failed body decode all come
/// back as k_ra8_err_validation_failed, and an over-cap dimension as
/// k_ra8_err_not_supported. Both mean the same thing to a caller of this
/// module -- these bytes cannot be drawn -- so they collapse onto
/// k_ra8_err_not_supported, keeping ra8_img_decode_blit()'s published
/// retval list unchanged. k_ra8_err_null_ptr and k_ra8_err_invalid_arg
/// pass through because they mean the same thing in both contracts.
///
/// One caveat this mapping cannot avoid: libwebp reports an exhausted
/// scratch arena as a plain decode failure, so a shortfall in the WebP
/// scratch surfaces as k_ra8_err_not_supported, not k_ra8_err_no_mem.
/// The canvas allocation, which dominates the arena footprint, is checked
/// separately in webpDecodeBlit() and does report no_mem.
fn webpClassify(err: c_int) c_int {
    if (err == k_ra8_ok or err == k_ra8_err_null_ptr or err == k_ra8_err_invalid_arg) {
        return err;
    }
    return k_ra8_err_not_supported;
}

/// Compute the aspect-preserving fit rectangle for an image in a box.
///
/// Picks the largest integer scale that maps the source into the box
/// while preserving aspect ratio. The tighter axis is determined by
/// comparing `box_w * src_h` with `box_h * src_w` using i64 products to
/// prevent overflow on large dimensions. Both output dimensions are
/// clamped to at least k_ra8_img_min_edge (1 pixel) so downstream callers
/// never receive a zero-size rectangle.
fn fitBox(src_w: i32, src_h: i32, box_w: i32, box_h: i32, fit_w: *i32, fit_h: *i32) void {
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

/// Map a decode failure to the closest ra8_err_t via stbi_failure_reason.
///
/// Queries stbi_failure_reason() immediately after a failed
/// stbi_load_from_memory() call and inspects the returned string for the
/// substring "outofmem". When that tag is present the arena exhausted its
/// capacity before the decode completed; the function returns
/// k_ra8_err_no_mem so the caller can report a memory shortage rather
/// than a format error. Any other reason string (corrupt header,
/// unsupported colour depth, unsupported format, etc.) and a null reason
/// string map to k_ra8_err_not_supported.
fn decodeFail() c_int {
    const reason = stbi_failure_reason() orelse return k_ra8_err_not_supported;
    if (std.mem.indexOf(u8, std.mem.span(reason), "outofmem") != null) {
        return k_ra8_err_no_mem;
    }
    return k_ra8_err_not_supported;
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
fn blitScaled(pixels: [*]const u8, src_w: i32, src_h: i32, src_bpp: i32, fit_w: i32, fit_h: i32, dst_x: i32, dst_y: i32, emit: PixelEmitter) void {
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

/// Unbind the decode arena and force it back to the fully-drained state.
///
/// Calls ra8_img_arena_unbind() to clear the module-static pointer that
/// redirects stb_image allocations, then resets both bookkeeping fields
/// of the arena struct to zero: `offset` (the bump pointer) and `live`
/// (the outstanding allocation count). This guarantees the arena is ready
/// for reuse and that no stale stb_image allocation callbacks can reach
/// it after the call. The `base` and `cap` fields, which are owned by the
/// caller, are left untouched. Called on every return path of
/// ra8_img_decode_blit().
fn arenaRelease(arena: *Ra8ImgArena) void {
    ra8_img_arena_unbind();
    arena.offset = 0;
    arena.live = 0;
}

/// Convert `px_count` tightly packed RGBA8888 pixels to 8-bit grayscale.
///
/// Uses the exact BT.601 integer coefficients used by stb_image (`(77*R +
/// 150*G + 29*B) >> 8`), discarding the alpha channel so PNG and WebP gray8
/// decodes agree byte-for-byte.
fn rgbaToGray8(rgba: [*]const u8, px_count: usize, dst: [*]u8) void {
    var i: usize = 0;
    while (i < px_count) : (i += 1) {
        const base = i * @as(usize, @intCast(k_ra8_img_req_rgba));
        const r: u32 = rgba[base + k_ra8_img_ch_r];
        const g: u32 = rgba[base + k_ra8_img_ch_g];
        const b: u32 = rgba[base + k_ra8_img_ch_b];
        dst[i] = @intCast((r * k_ra8_luma_r + g * k_ra8_luma_g + b * k_ra8_luma_b) >> k_ra8_luma_shift);
    }
}

/// Decode a WebP container into an arena-backed RGBA8888 canvas.
///
/// Shared backend for both `webpDecodeBlit` and `webpDecodeGray8`. When
/// `dst_px_cap` is provided, verifies the caller's pixel capacity before
/// binding `arena`. On `k_ra8_ok`, `arena` remains bound until the caller
/// invokes `arenaRelease(arena)`; on any error, `arena` is already released.
fn webpDecodeRgba(
    arena: *Ra8ImgArena,
    bytes: [*]const u8,
    len: usize,
    dst_px_cap: ?usize,
    out_w: *u32,
    out_h: *u32,
    out_rgba: *[*]u8,
) c_int {
    var src_w: u32 = 0;
    var src_h: u32 = 0;
    const info = ra8_webp_get_info(bytes, len, &src_w, &src_h);
    if (info != k_ra8_ok) {
        logErr("img: webp header rejected");
        return webpClassify(info);
    }
    if (src_w == 0 or src_h == 0) {
        logErr("img: webp empty dimensions");
        return k_ra8_err_not_supported;
    }
    const px_count = std.math.mul(usize, src_w, src_h) catch {
        logErr("img: webp pixel count overflow");
        return k_ra8_err_no_mem;
    };
    if (dst_px_cap) |cap| {
        if (cap < px_count) {
            logErr("img: webp dst capacity too small");
            return k_ra8_err_no_mem;
        }
    }
    const stride = std.math.mul(usize, src_w, @as(usize, @intCast(k_ra8_img_req_rgba))) catch {
        return k_ra8_err_no_mem;
    };
    const canvas = std.math.mul(usize, src_h, stride) catch {
        return k_ra8_err_no_mem;
    };

    ra8_img_arena_bind(arena); // resets the arena to empty
    const rgba_block = ra8_img_arena_malloc(canvas) orelse {
        arenaRelease(arena);
        logErr("img: webp canvas does not fit the arena");
        return k_ra8_err_no_mem;
    };
    const rgba: [*]u8 = @ptrCast(@alignCast(rgba_block));

    // Whatever the canvas did not take backs libwebp's own scratch.
    var scratch = Ra8ImgArena{
        .base = arena.base.? + arena.offset,
        .cap = arena.cap - arena.offset,
        .offset = 0,
        .live = 0,
        .high_water = 0,
    };
    const dec = ra8_webp_decode_rgba(bytes, len, &scratch, rgba, stride, canvas, null, null);
    if (dec != k_ra8_ok) {
        arenaRelease(arena);
        logErr("img: webp decode failed");
        return webpClassify(dec);
    }

    out_w.* = src_w;
    out_h.* = src_h;
    out_rgba.* = rgba;
    return k_ra8_ok;
}

/// Decode a WebP through the ra8_webp facade and blit it scaled to fit.
fn webpDecodeBlit(arena: *Ra8ImgArena, bytes: [*]const u8, len: usize, dst_x: i32, dst_y: i32, box_w: i32, box_h: i32, out_w: ?*i32, out_h: ?*i32) c_int {
    var src_w: u32 = 0;
    var src_h: u32 = 0;
    var rgba: [*]u8 = undefined;
    const status = webpDecodeRgba(arena, bytes, len, null, &src_w, &src_h, &rgba);
    if (status != k_ra8_ok) {
        return status;
    }

    var fit_w: i32 = 0;
    var fit_h: i32 = 0;
    fitBox(@intCast(src_w), @intCast(src_h), box_w, box_h, &fit_w, &fit_h);
    blitScaled(rgba, @intCast(src_w), @intCast(src_h), k_ra8_img_req_rgba, fit_w, fit_h, dst_x, dst_y, gfxEmit);
    arenaRelease(arena);

    if (out_w) |p| {
        p.* = fit_w;
    }
    if (out_h) |p| {
        p.* = fit_h;
    }
    return k_ra8_ok;
}

/// Decode a WebP container into a tightly packed gray8 buffer via `arena`.
fn webpDecodeGray8(arena: *Ra8ImgArena, bytes: [*]const u8, len: usize, dst: [*]u8, dst_cap: usize, out_w: *u32, out_h: *u32) c_int {
    var src_w: u32 = 0;
    var src_h: u32 = 0;
    var rgba: [*]u8 = undefined;
    const status = webpDecodeRgba(arena, bytes, len, dst_cap, &src_w, &src_h, &rgba);
    if (status != k_ra8_ok) {
        return status;
    }

    const px_count: usize = @as(usize, src_w) * @as(usize, src_h);
    rgbaToGray8(rgba, px_count, dst);
    arenaRelease(arena);
    out_w.* = src_w;
    out_h.* = src_h;
    return k_ra8_ok;
}

/// Decode an stb_image raster directly to 1-channel gray8 via `arena`.
fn stbiDecodeGray8(arena: *Ra8ImgArena, bytes: [*]const u8, len: usize, dst: [*]u8, dst_cap: usize, out_w: *u32, out_h: *u32) c_int {
    ra8_img_arena_bind(arena); // resets the arena to empty
    var sx: c_int = 0;
    var sy: c_int = 0;
    var comp: c_int = 0;
    const pixels = stbi_load_from_memory(bytes, @intCast(len), &sx, &sy, &comp, k_ra8_img_req_gray);
    if ((pixels == null) or (sx <= 0) or (sy <= 0)) {
        const err = decodeFail();
        arenaRelease(arena);
        logErr("gray8: decode failed");
        return err;
    }

    const w: u32 = @intCast(sx);
    const h: u32 = @intCast(sy);
    const px_count = std.math.mul(usize, w, h) catch {
        stbi_image_free(@ptrCast(pixels.?));
        arenaRelease(arena);
        return k_ra8_err_no_mem;
    };
    if (dst_cap < px_count) {
        stbi_image_free(@ptrCast(pixels.?));
        arenaRelease(arena);
        logErr("gray8: dst buffer too small");
        return k_ra8_err_no_mem;
    }
    @memcpy(dst[0..px_count], pixels.?[0..px_count]);
    stbi_image_free(@ptrCast(pixels.?));
    arenaRelease(arena);
    out_w.* = w;
    out_h.* = h;
    return k_ra8_ok;
}

/// Probe an image's intrinsic dimensions without a full decode.
///
/// Lets the layout pass size an `<img>` box from the source dimensions
/// before deciding to decode + blit. Allocates nothing. SVG is not a
/// raster: its intrinsic size comes from the document. stb_image cannot
/// parse a WebP header; the ra8_webp facade can.
export fn ra8_img_probe_size(bytes: ?[*]const u8, len: usize, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
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

    // SVG is not a raster: take its intrinsic size from the document.
    if (ra8_svg_is_svg(b, len)) {
        return ra8_svg_size(b, len, out_w.?, out_h.?);
    }

    // stb_image cannot parse a WebP header; the ra8_webp facade can.
    if (isWebp(b, len)) {
        var webp_w: u32 = 0;
        var webp_h: u32 = 0;
        const err = ra8_webp_get_info(b, len, &webp_w, &webp_h);
        if (err != k_ra8_ok) {
            logErr("probe: webp header rejected");
            return webpClassify(err);
        }
        out_w.?.* = @intCast(webp_w);
        out_h.?.* = @intCast(webp_h);
        return k_ra8_ok;
    }

    var x: c_int = 0;
    var y: c_int = 0;
    var comp: c_int = 0;
    if (stbi_info_from_memory(b, @intCast(len), &x, &y, &comp) == 0) {
        logErr("probe: stbi_info rejected the header");
        return k_ra8_err_not_supported;
    }
    out_w.?.* = x;
    out_h.?.* = y;
    return k_ra8_ok;
}

/// Decode `bytes` and blit it, scaled to fit, into the bound framebuffer.
///
/// Decodes the image to RGB through stb_image (allocating only from
/// `arena`), computes a fit rectangle inside `box_w x box_h` that
/// preserves the source aspect ratio, then nearest-neighbour blits the
/// scaled image at `(dst_x, dst_y)` via ra8_gfx. The arena is fully reset
/// on return (success or failure). The caller must have bound a
/// framebuffer with ra8_gfx_init().
export fn ra8_img_decode_blit(arena: ?*Ra8ImgArena, bytes: ?[*]const u8, len: usize, dst_x: i32, dst_y: i32, box_w: i32, box_h: i32, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
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

    // stb_image has no WebP decoder: route the container to libwebp.
    if (isWebp(b, len)) {
        return webpDecodeBlit(a, b, len, dst_x, dst_y, box_w, box_h, out_w, out_h);
    }

    ra8_img_arena_bind(a); // resets the arena to empty
    var sx: c_int = 0;
    var sy: c_int = 0;
    var comp: c_int = 0;
    // stbi_load_from_memory() returns a non-null pixel pointer only when it
    // decoded a bitmap of at least 1x1 (it rejects zero-dimension images
    // with a null return), so whenever pixels != null, sx >= 1 and sy >= 1.
    const pixels = stbi_load_from_memory(b, @intCast(len), &sx, &sy, &comp, k_ra8_img_req_rgb);
    if ((pixels == null) or (sx <= 0) or (sy <= 0)) {
        const err = decodeFail();
        arenaRelease(a);
        logErr("blit: decode failed");
        return err;
    }

    var fit_w: i32 = 0;
    var fit_h: i32 = 0;
    fitBox(sx, sy, box_w, box_h, &fit_w, &fit_h);
    blitScaled(pixels.?, sx, sy, k_ra8_img_req_rgb, fit_w, fit_h, dst_x, dst_y, gfxEmit);
    stbi_image_free(@ptrCast(pixels.?));
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
export fn ra8_img_decode_gray8(
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
    if (isWebp(b, len)) {
        return webpDecodeGray8(a, b, len, d, dst_cap, out_w.?, out_h.?);
    }
    return stbiDecodeGray8(a, b, len, d, dst_cap, out_w.?, out_h.?);
}

// ---- Unit tests (pure logic; the decode stack is answered by the
// ---- test-only link seam, which returns "not supported" so the export
// ---- paths fall through to the same outcomes the C-era harness saw) ----

const testing = std.testing;

test "fit box fills the tighter width axis" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    // box_w*src_h (40*50) <= box_h*src_w (100*100): width is tighter.
    fitBox(100, 50, 40, 100, &fw, &fh);
    try testing.expectEqual(@as(i32, 40), fw);
    try testing.expectEqual(@as(i32, 20), fh);
}

test "fit box fills the tighter height axis" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    // box_w*src_h (100*100) > box_h*src_w (40*50): height is tighter.
    fitBox(50, 100, 100, 40, &fw, &fh);
    try testing.expectEqual(@as(i32, 20), fw);
    try testing.expectEqual(@as(i32, 40), fh);
}

test "fit box clamps degenerate scales to the minimum edge" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    // A 1000x2 source in a 10x10 box scales height to 0; clamped to 1.
    fitBox(1000, 2, 10, 10, &fw, &fh);
    try testing.expectEqual(@as(i32, 10), fw);
    try testing.expectEqual(@as(i32, 1), fh);
}

test "fit box uses i64 products on large dimensions" {
    var fw: i32 = 0;
    var fh: i32 = 0;
    // 60000 * 60000 overflows i32 products; the i64 path must not.
    fitBox(60000, 60000, 30000, 30000, &fw, &fh);
    try testing.expectEqual(@as(i32, 30000), fw);
    try testing.expectEqual(@as(i32, 30000), fh);
}

const t_max_emit = 256;
var t_xy: [t_max_emit][2]i32 = undefined;
var t_color: [t_max_emit]u32 = undefined;
var t_count: usize = 0;

fn tEmit(x: i32, y: i32, color: u32) void {
    if (t_count < t_max_emit) {
        t_xy[t_count] = .{ x, y };
        t_color[t_count] = color;
    }
    t_count += 1;
}

fn tReset() void {
    t_count = 0;
}

test "blit maps 1:1 for an unscaled image and packs 0x00RRGGBB" {
    // 2x2 RGB source blitted 1:1 at (5, 7).
    const px = [_]u8{
        0x10, 0x20, 0x30, // pixel (0,0)
        0x40, 0x50, 0x60, // pixel (1,0)
        0x70, 0x80, 0x90, // pixel (0,1)
        0xa0, 0xb0, 0xc0, // pixel (1,1)
    };
    tReset();
    blitScaled(&px, 2, 2, 3, 2, 2, 5, 7, tEmit);
    try testing.expectEqual(@as(usize, 4), t_count);
    try testing.expectEqual([2]i32{ 5, 7 }, t_xy[0]);
    try testing.expectEqual([2]i32{ 6, 7 }, t_xy[1]);
    try testing.expectEqual([2]i32{ 5, 8 }, t_xy[2]);
    try testing.expectEqual([2]i32{ 6, 8 }, t_xy[3]);
    try testing.expectEqual(@as(u32, 0x102030), t_color[0]);
    try testing.expectEqual(@as(u32, 0x405060), t_color[1]);
    try testing.expectEqual(@as(u32, 0x708090), t_color[2]);
    try testing.expectEqual(@as(u32, 0xa0b0c0), t_color[3]);
}

test "blit picks nearest-neighbour source columns when downscaling" {
    // 4x1 source scaled into 2x1: map_x = dx * 4 / 2 -> 0 and 2.
    const px = [_]u8{
        1, 2, 3, // source pixel 0
        4, 5, 6, // source pixel 1
        7, 8, 9, // source pixel 2
        10, 11, 12, // source pixel 3
    };
    tReset();
    blitScaled(&px, 4, 1, 3, 2, 1, 0, 0, tEmit);
    try testing.expectEqual(@as(usize, 2), t_count);
    try testing.expectEqual(@as(u32, 0x010203), t_color[0]);
    try testing.expectEqual(@as(u32, 0x070809), t_color[1]);
}

test "blit strides by src_bpp for RGBA sources" {
    // 1x2 RGBA source (bpp 4) scaled 1:1: the alpha byte is ignored.
    const px = [_]u8{
        0x11, 0x22, 0x33, 0xff, // pixel (0,0)
        0x44, 0x55, 0x66, 0x80, // pixel (0,1)
    };
    tReset();
    blitScaled(&px, 1, 2, 4, 1, 2, 0, 0, tEmit);
    try testing.expectEqual(@as(usize, 2), t_count);
    try testing.expectEqual(@as(u32, 0x112233), t_color[0]);
    try testing.expectEqual(@as(u32, 0x445566), t_color[1]);
}

test "webp classification passes the shared statuses through" {
    try testing.expectEqual(k_ra8_ok, webpClassify(k_ra8_ok));
    try testing.expectEqual(k_ra8_err_null_ptr, webpClassify(k_ra8_err_null_ptr));
    try testing.expectEqual(k_ra8_err_invalid_arg, webpClassify(k_ra8_err_invalid_arg));
    try testing.expectEqual(k_ra8_err_not_supported, webpClassify(k_ra8_err_not_supported));
    try testing.expectEqual(k_ra8_err_not_supported, webpClassify(k_ra8_err_no_mem));
    try testing.expectEqual(k_ra8_err_not_supported, webpClassify(0x105));
}

test "decode blit rejects a null arena and null bytes" {
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_blit(null, "x", 1, 0, 0, 1, 1, null, null));
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_blit(&scratch, null, 1, 0, 0, 1, 1, null, null));
}

test "decode blit rejects an empty input or an empty box" {
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_img_decode_blit(&scratch, "x", 0, 0, 0, 1, 1, null, null));
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 0, 1, null, null));
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 1, 0, null, null));
}

test "decode blit drains the arena on the decode-failure path" {
    // The seam answers stbi_load_from_memory with null, so the export
    // falls to the decode-failure path; the arena must come back drained.
    var backing: [64]u8 = undefined;
    var scratch = Ra8ImgArena{
        .base = &backing,
        .cap = backing.len,
        .offset = 24,
        .live = 3,
        .high_water = 24,
    };
    try testing.expectEqual(k_ra8_err_not_supported, ra8_img_decode_blit(&scratch, "x", 1, 0, 0, 1, 1, null, null));
    try testing.expectEqual(@as(usize, 0), scratch.offset);
    try testing.expectEqual(@as(usize, 0), scratch.live);
    try testing.expectEqual(@as(usize, 24), scratch.high_water);
}

test "probe rejects the seam's not-supported stack" {
    var w: i32 = 0;
    var h: i32 = 0;
    // The seam answers SVG=no, sniff=fail, stbi_info=reject: not supported.
    try testing.expectEqual(k_ra8_err_not_supported, ra8_img_probe_size("x", 1, &w, &h));
}

test "probe rejects null arguments" {
    var w: i32 = 0;
    var h: i32 = 0;
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_probe_size(null, 1, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_probe_size("x", 1, null, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_probe_size("x", 1, &w, null));
}

test "rgbaToGray8 matches BT.601 integer luma and ignores alpha" {
    const rgba = [_]u8{
        255, 0, 0, 0x11, // red
        0, 255, 0, 0x22, // green
        0, 0, 255, 0x33, // blue
        255, 255, 255, 0x00, // white, alpha=0
    };
    var gray: [4]u8 = undefined;
    rgbaToGray8(&rgba, 4, &gray);
    try testing.expectEqual([4]u8{ 76, 149, 28, 255 }, gray);
}

test "decode gray8 rejects null arguments and invalid sizes" {
    var scratch = Ra8ImgArena{ .base = null, .cap = 16, .offset = 0, .live = 0, .high_water = 0 };
    var dst: [4]u8 = undefined;
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_gray8(null, "x", 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_gray8(&scratch, null, 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_gray8(&scratch, "x", 1, null, dst.len, &w, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, null, &h));
    try testing.expectEqual(k_ra8_err_null_ptr, ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, &w, null));
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_img_decode_gray8(&scratch, "x", 0, &dst, dst.len, &w, &h));
    try testing.expectEqual(k_ra8_err_invalid_arg, ra8_img_decode_gray8(&scratch, "x", 1, &dst, 0, &w, &h));
}

test "decode gray8 drains the arena on the decode-failure path" {
    var backing: [64]u8 = undefined;
    var scratch = Ra8ImgArena{
        .base = &backing,
        .cap = backing.len,
        .offset = 16,
        .live = 2,
        .high_water = 16,
    };
    var dst: [4]u8 = undefined;
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_err_not_supported, ra8_img_decode_gray8(&scratch, "x", 1, &dst, dst.len, &w, &h));
    try testing.expectEqual(@as(usize, 0), scratch.offset);
    try testing.expectEqual(@as(usize, 0), scratch.live);
    try testing.expectEqual(@as(usize, 16), scratch.high_water);
}
