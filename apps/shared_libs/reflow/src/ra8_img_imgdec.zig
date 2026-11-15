//! `ra8_imgdec` backend over the vendored stb_image residue
//! (src/ra8_img_imgdec.c, RA8FW-1232).
//!
//! Zig port of the vtable, the caps record and the decode hook for
//! ra8_img_imgdec_bind(). The hook is a thin adapter: it re-checks the
//! container it was told it holds, pre-flights the geometry, runs the
//! existing stbi_load_from_memory() call against the bound arena, and
//! copies the result into the caller's surface. No decoding logic is
//! added or duplicated here.
//!
//! stb_image, the ra8_imgdec fabric, the arena hooks and ra8_log stay
//! behind the extern seam; ra8_img_imgdec_bind keeps C linkage so the
//! mux and the C callers link against it exactly as before.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");

// ---- Error codes (libs/ra8_core/inc/ra8_err.h; ra8_err_t is a plain
// ---- c_int in the C ABI, so these mirror the C enumerators) ----

const k_ra8_ok: c_int = 0;
const k_ra8_err_no_mem: c_int = 0x102;
const k_ra8_err_invalid_state: c_int = 0x104;
const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_supported: c_int = 0x107;
const k_ra8_err_validation_failed: c_int = 0x501;
const k_ra8_err_null_ptr: c_int = 0x504;

// ---- Format / pixel bits (libs/ra8_imgdec/inc/ra8_imgdec.h) ----

const k_ra8_imgdec_format_png: u32 = 1 << 1;
const k_ra8_imgdec_format_gif: u32 = 1 << 3;
const k_ra8_imgdec_format_bmp: u32 = 1 << 4;
const k_ra8_imgdec_pixel_grey8: u32 = 1 << 0;
const k_ra8_imgdec_pixel_rgb888: u32 = 1 << 1;
const k_ra8_imgdec_pixel_rgba8888: u32 = 1 << 2;

// ---- Numbers this backend publishes about itself ----

/// STBI_MAX_DIMENSIONS in stb_image_impl.c.
const k_stbdec_dim_max: u32 = 8192;
/// stb channel count: grey + alpha.
const k_stbdec_alpha_g: i32 = 2;
/// stb channel count: RGB + alpha.
const k_stbdec_alpha_c: i32 = 4;
/// Formats this backend advertises. PNG is on the list because stb is
/// the only bindable PNG decoder in the tree until libs/ra8_png exists;
/// JPEG stays off it (the first-party codec is already bound).
const k_ra8_img_imgdec_formats: u32 =
    k_ra8_imgdec_format_png | k_ra8_imgdec_format_gif | k_ra8_imgdec_format_bmp;
/// Destination layouts stb can be asked for directly.
const k_ra8_img_imgdec_pixels: u32 =
    k_ra8_imgdec_pixel_grey8 | k_ra8_imgdec_pixel_rgb888 | k_ra8_imgdec_pixel_rgba8888;
/// stb_image takes its input length as an int, so the request's byte
/// count is refused above INT_MAX rather than truncated into a short read.
const k_int32_max: u32 = 0x7FFF_FFFF;

comptime {
    // The whole surface is measured in 32 bits, so the widest frame this
    // backend admits must fit there at its fattest layout.
    std.debug.assert(@as(u64, k_stbdec_dim_max) * @as(u64, k_stbdec_dim_max) * 4 <= 0xFFFFFFFF);
    std.debug.assert(@sizeOf(c_int) >= 4);
}

// ---- Extern seam: the decode stack stays C ----

// stb_image (apps/shared_libs/third_party/stb/stb_image.h).
extern fn stbi_load_from_memory(buffer: [*]const u8, len: c_int, x: *c_int, y: *c_int, channels_in_file: *c_int, desired_channels: c_int) callconv(.c) ?[*]u8;
extern fn stbi_failure_reason() callconv(.c) ?[*:0]const u8;
extern fn stbi_image_free(retval_from_stbi_load: *anyopaque) callconv(.c) void;

// ra8_imgdec fabric (libs/ra8_imgdec).
extern fn ra8_imgdec_dims(bytes: [*]const u8, byte_count: u32, out: *Ra8ImgdecGeom) callconv(.c) c_int;
extern fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32;

// ra8_img_arena bump allocator (apps/shared_libs/reflow/inc/ra8_img_arena.h).
extern fn ra8_img_arena_bind(arena: *Ra8ImgArena) callconv(.c) void;
extern fn ra8_img_arena_unbind() callconv(.c) void;

// ra8_log backend entry (ra8_log_error() expands to this in ra8_log.h).
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;

// ---- Hand-written extern mirrors (ra8_imgdec.h / ra8_imgdec_backend.h) ----

/// One spelling of ra8_imgdec_scratch_t: the ra8_img_arena_t alias shares it.
pub const Ra8ImgArena = extern struct {
    base: ?[*]u8,
    cap: usize,
    offset: usize,
    live: usize,
    high_water: usize,
};

/// Mirror of ra8_imgdec_req_t.
pub const Ra8ImgdecReq = extern struct {
    bytes: ?[*]const u8,
    byte_count: u32,
    arena: ?*anyopaque,
    dst: ?[*]u8,
    dst_bytes: u32,
    dst_stride: u32,
    format: u32,
    want: u32,
};

/// Mirror of ra8_imgdec_caps_t.
pub const Ra8ImgdecCaps = extern struct {
    formats: u32,
    pixels: u32,
    scratch_bytes: u32,
    scratch_align: u32,
    dim_max: u32,
    streams: bool,
};

/// Mirror of ra8_imgdec_image_t.
pub const Ra8ImgdecImage = extern struct {
    width_px: u32,
    height_px: u32,
    stride: u32,
    used_bytes: u32,
    format: u32,
    pixel: u32,
    had_alpha: bool,
};

/// Mirror of ra8_imgdec_geom_t.
pub const Ra8ImgdecGeom = extern struct {
    format: u32,
    width_px: u32,
    height_px: u32,
};

/// Mirror of ra8_imgdec_iface (ra8_imgdec_backend.h).
pub const Ra8ImgdecIface = extern struct {
    get_caps: *const fn (?*anyopaque, *Ra8ImgdecCaps) callconv(.c) c_int,
    decode: *const fn (?*anyopaque, *const Ra8ImgdecReq, *Ra8ImgdecImage) callconv(.c) c_int,
};

/// Mirror of ra8_imgdec_t.
pub const Ra8Imgdec = extern struct {
    iface: ?*const Ra8ImgdecIface,
    ctx: ?*anyopaque,
};

/// Module log tag.
const s_tag_stbdec: [*:0]const u8 = "ra8_img_imgdec";

fn logErr(message: [*:0]const u8) void {
    ra8_log_emit_error(s_tag_stbdec, message);
}

// ---- Internal helpers ----

/// Verify the container and read its declared geometry, in one pass:
/// both duties the fabric leaves to a backend are answered by
/// ra8_imgdec_dims(), so this asks it once rather than sniffing and then
/// probing. The container check is not ceremony: stb_image decodes JPEG
/// as happily as the three formats above, so bytes that are secretly a
/// JPEG would otherwise decode fine through a handle that does not
/// advertise it. stbi_info_from_memory() is deliberately NOT the probe
/// here: it reports failure on a GIF this backend decodes without
/// complaint, so pre-flighting through it would refuse a whole
/// advertised format.
fn internalProbe(req: *const Ra8ImgdecReq, out_w: *u32, out_h: *u32) c_int {
    var geom: Ra8ImgdecGeom = std.mem.zeroes(Ra8ImgdecGeom);
    const err = ra8_imgdec_dims(req.bytes.?, req.byte_count, &geom);
    if (err != k_ra8_ok) {
        return err;
    }
    if (geom.format != req.format) {
        return k_ra8_err_not_supported;
    }
    if ((geom.width_px > k_stbdec_dim_max) or (geom.height_px > k_stbdec_dim_max)) {
        return k_ra8_err_invalid_size;
    }
    out_w.* = geom.width_px;
    out_h.* = geom.height_px;
    return k_ra8_ok;
}

/// Decide whether the request's destination can take the surface.
/// Unlike the first-party JPEG backend, this one is free to honour a
/// padded dst_stride: it copies row by row out of stb's own buffer, so
/// a wider stride costs an offset per row and nothing else. Only a
/// stride narrower than one packed row is impossible to satisfy. The
/// last row occupies a packed row, not a strided one: a destination
/// sized exactly to the image must not be refused over padding it never
/// uses.
fn internalDstOk(req: *const Ra8ImgdecReq, rows: u32, packed_row: u32, out_need: *u32) c_int {
    const stride: u32 = if (req.dst_stride != 0) req.dst_stride else packed_row;
    if (stride < packed_row) {
        return k_ra8_err_invalid_size;
    }
    const need: u64 = (@as(u64, rows) - 1) * @as(u64, stride) + @as(u64, packed_row);
    if (need > @as(u64, req.dst_bytes)) {
        return k_ra8_err_invalid_size;
    }
    out_need.* = @intCast(need);
    return k_ra8_ok;
}

/// Copy stb's packed surface into the caller's (possibly padded) one.
fn internalCopyRows(pixels: [*]const u8, dst: [*]u8, stride: u32, rows: u32, packed_row: u32) void {
    var y: u32 = 0;
    while (y < rows) : (y += 1) {
        const dst_row = dst + @as(usize, y) * @as(usize, stride);
        const src_row = pixels + @as(usize, y) * @as(usize, packed_row);
        @memcpy(dst_row[0..packed_row], src_row[0..packed_row]);
    }
}

/// Map a failed stb decode to the closest ra8_err_t. stbi_load_from_memory()
/// returns one null pointer for two quite different events: the arena ran
/// out mid-decode, or the body is corrupt. The reason string is the only
/// thing that tells them apart, and the distinction matters to a
/// consumer: a shortage is fixed by handing the binder a bigger arena, a
/// corrupt body never is. Same classification ra8_img_decode_blit() has
/// always done, applied at the seam instead of inside the blit.
fn internalDecodeFail() c_int {
    const reason = stbi_failure_reason();
    if (reason) |r| {
        if (std.mem.indexOf(u8, std.mem.span(r), "outofmem") != null) {
            return k_ra8_err_no_mem;
        }
    }
    return k_ra8_err_not_supported;
}

/// Unbind the arena and return it to empty. Called on every path out of
/// the decode once the arena has been bound, so no handle is left
/// pointing at the previous decode's store and no stray allocation can
/// reach a stale buffer.
fn internalArenaRelease(arena: *Ra8ImgArena) void {
    ra8_img_arena_unbind();
    arena.offset = 0;
    arena.live = 0;
}

/// Report this backend's static capabilities. The arena came through the
/// binder, not the request, so scratch_bytes is the literal truth about
/// req->arena: this backend draws nothing from it.
fn internalCaps(ctx: ?*anyopaque, out: *Ra8ImgdecCaps) callconv(.c) c_int {
    _ = ctx;
    out.formats = k_ra8_img_imgdec_formats;
    out.pixels = k_ra8_img_imgdec_pixels;
    out.scratch_bytes = 0;
    out.scratch_align = 0;
    out.dim_max = k_stbdec_dim_max;
    out.streams = false;
    return k_ra8_ok;
}

/// Decode one PNG, GIF or BMP into the request's destination surface.
/// Verifies the container, pre-flights the geometry and the destination,
/// then binds the arena recorded at ra8_img_imgdec_bind() time and calls
/// stbi_load_from_memory() asking for exactly the layout req->want
/// names. stb's buffer is copied into req->dst and the arena is drained
/// on every return path, success or failure, so a handle is never left
/// holding the previous decode's store. The probe read the header; stb
/// read it again on its way through the body. They must agree, and a
/// disagreement means the copy below would run off the end of one
/// buffer or the other, so it is refused rather than clamped.
fn internalDecode(ctx: ?*anyopaque, req: *const Ra8ImgdecReq, out: *Ra8ImgdecImage) callconv(.c) c_int {
    const arena_ptr = ctx orelse return k_ra8_err_invalid_state;
    const arena: *Ra8ImgArena = @ptrCast(@alignCast(arena_ptr));
    if (req.byte_count > k_int32_max) {
        return k_ra8_err_invalid_size;
    }

    var src_w: u32 = 0;
    var src_h: u32 = 0;
    const geom = internalProbe(req, &src_w, &src_h);
    if (geom != k_ra8_ok) {
        logErr("decode: header rejected");
        return geom;
    }

    const bpp = ra8_imgdec_pixel_bytes(req.want);
    const packed_row = src_w * bpp;
    var need: u32 = 0;
    const fits = internalDstOk(req, src_h, packed_row, &need);
    if (fits != k_ra8_ok) {
        logErr("decode: destination cannot hold the surface");
        return fits;
    }

    var sx: c_int = 0;
    var sy: c_int = 0;
    var comp: c_int = 0;
    ra8_img_arena_bind(arena); // resets the arena to empty
    const pixels = stbi_load_from_memory(req.bytes.?, @intCast(req.byte_count), &sx, &sy, &comp, @intCast(bpp));
    if (pixels == null) {
        const why = internalDecodeFail();
        internalArenaRelease(arena);
        logErr("decode: stb refused the image");
        return why;
    }

    if ((sx != @as(c_int, @intCast(src_w))) or (sy != @as(c_int, @intCast(src_h)))) {
        internalArenaRelease(arena);
        logErr("decode: decoded geometry disagrees with the header");
        return k_ra8_err_validation_failed;
    }

    const stride: u32 = if (req.dst_stride != 0) req.dst_stride else packed_row;
    internalCopyRows(pixels.?, req.dst.?, stride, src_h, packed_row);

    stbi_image_free(@ptrCast(pixels.?));
    internalArenaRelease(arena);

    out.width_px = src_w;
    out.height_px = src_h;
    out.stride = stride;
    out.used_bytes = need;
    out.format = req.format;
    out.pixel = req.want;
    out.had_alpha = (comp == k_stbdec_alpha_g) or (comp == k_stbdec_alpha_c);
    return k_ra8_ok;
}

/// The one vtable instance; never per handle.
const s_iface: Ra8ImgdecIface = .{ .get_caps = internalCaps, .decode = internalDecode };

// ---- Public entry point ----

/// Implementation of ra8_img_imgdec_bind() (inc/ra8_img_imgdec.h): fill
/// out with this module's vtable and record scratch as the arena every
/// decode through out will bump.
export fn ra8_img_imgdec_bind(out: ?*Ra8Imgdec, scratch: ?*Ra8ImgArena) callconv(.c) c_int {
    if (out == null) {
        logErr("bind: null handle");
        return k_ra8_err_null_ptr;
    }
    if (scratch == null) {
        logErr("bind: null scratch");
        return k_ra8_err_null_ptr;
    }
    out.?.iface = &s_iface;
    out.?.ctx = scratch;
    return k_ra8_ok;
}

// ---- Unit tests (pure logic; the decode stack is answered by the
// ---- test-only link seam, which rejects every header, so the hook
// ---- paths fall through to the outcomes the C-era harness saw) ----

const testing = std.testing;

test "destination check honours a padded stride" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 24,
        .dst_stride = 10,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    // 2 rows of 8 packed bytes at stride 10: (2-1)*10 + 8 = 18 <= 24.
    try testing.expectEqual(k_ra8_ok, internalDstOk(&req, 2, 8, &need));
    try testing.expectEqual(@as(u32, 18), need);
}

test "destination check refuses a stride under one packed row" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 64,
        .dst_stride = 7,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(k_ra8_err_invalid_size, internalDstOk(&req, 2, 8, &need));
}

test "destination check refuses a destination under the exact span" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 17,
        .dst_stride = 10,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(k_ra8_err_invalid_size, internalDstOk(&req, 2, 8, &need));
}

test "destination check accepts an exactly-sized destination" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 18,
        .dst_stride = 0,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(k_ra8_ok, internalDstOk(&req, 2, 9, &need));
    try testing.expectEqual(@as(u32, 18), need);
}

test "caps advertise the stb residue" {
    var caps: Ra8ImgdecCaps = undefined;
    try testing.expectEqual(k_ra8_ok, internalCaps(null, &caps));
    try testing.expectEqual(k_ra8_imgdec_format_png | k_ra8_imgdec_format_gif | k_ra8_imgdec_format_bmp, caps.formats);
    try testing.expectEqual(k_ra8_imgdec_pixel_grey8 | k_ra8_imgdec_pixel_rgb888 | k_ra8_imgdec_pixel_rgba8888, caps.pixels);
    try testing.expectEqual(@as(u32, 0), caps.scratch_bytes);
    try testing.expectEqual(@as(u32, 0), caps.scratch_align);
    try testing.expectEqual(@as(u32, 8192), caps.dim_max);
    try testing.expectEqual(false, caps.streams);
}

test "mirror layout sanity" {
    comptime {
        std.debug.assert(@offsetOf(Ra8ImgdecReq, "byte_count") == @sizeOf(?[*]const u8));
        std.debug.assert(@offsetOf(Ra8Imgdec, "ctx") == @sizeOf(?*const Ra8ImgdecIface));
    }
}
