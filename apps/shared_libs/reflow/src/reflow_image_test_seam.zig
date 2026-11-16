//! Test-only link seam for the reflow Zig unit test harness (RA8FW-1231).
//!
//! The real image decode stack -- the vendored stb_image, the ra8_webp
//! facade over libwebp, the shared ra8_imgdec container sniff, the
//! ra8_img_arena bump allocator, the SVG document surface and the ra8_log
//! backend -- is out of scope for this unit harness (the CMake suite
//! exercises it end to end). The real raster surface itself, though, now
//! ships in reflow_image.zig, so its exports (ra8_img_probe_size /
//! ra8_img_decode_blit) link here and the harness answers the stack they
//! reach with the documented "not supported" / empty results: the probe
//! falls through to not-supported and the decode falls to the
//! decode-failure path, the same outcomes the C-era stub seams produced.
//! The seam must never be imported by reflow_parse_abi.zig: the archive
//! keeps no stub.
const k_ra8_err_not_supported: c_int = 0x107;

const imgdec = @import("ra8_img_imgdec.zig");

// stb_image: reject every header / decode; the reason string stays unset.
export fn stbi_info_from_memory(buffer: [*]const u8, len: c_int, x: *c_int, y: *c_int, comp: *c_int) callconv(.c) c_int {
    _ = buffer;
    _ = len;
    _ = x;
    _ = y;
    _ = comp;
    return 0;
}

export fn stbi_load_from_memory(buffer: [*]const u8, len: c_int, x: *c_int, y: *c_int, channels_in_file: *c_int, desired_channels: c_int) callconv(.c) ?[*]u8 {
    _ = buffer;
    _ = len;
    _ = x;
    _ = y;
    _ = channels_in_file;
    _ = desired_channels;
    return null;
}

export fn stbi_failure_reason() callconv(.c) ?[*:0]const u8 {
    return null;
}

export fn stbi_image_free(retval_from_stbi_load: *anyopaque) callconv(.c) void {
    _ = retval_from_stbi_load;
}

// Shared container sniff: recognise nothing.
export fn ra8_imgdec_sniff(bytes: [*]const u8, byte_count: u32, out: *u32) callconv(.c) c_int {
    _ = bytes;
    _ = byte_count;
    _ = out;
    return k_ra8_err_not_supported;
}

// ra8_webp facade: reject every header / body.
export fn ra8_webp_get_info(data: [*]const u8, size: usize, out_w: *u32, out_h: *u32) callconv(.c) c_int {
    _ = data;
    _ = size;
    _ = out_w;
    _ = out_h;
    return k_ra8_err_not_supported;
}

export fn ra8_webp_decode_rgba(data: [*]const u8, size: usize, arena: *anyopaque, out_rgba: [*]u8, out_stride: usize, out_capacity: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) c_int {
    _ = data;
    _ = size;
    _ = arena;
    _ = out_rgba;
    _ = out_stride;
    _ = out_capacity;
    _ = out_w;
    _ = out_h;
    return k_ra8_err_not_supported;
}

// ra8_img_arena: bind / unbind are no-ops, malloc always exhausts.
export fn ra8_img_arena_bind(arena: *anyopaque) callconv(.c) void {
    _ = arena;
}

export fn ra8_img_arena_unbind() callconv(.c) void {}

export fn ra8_img_arena_malloc(n: usize) callconv(.c) ?*anyopaque {
    _ = n;
    return null;
}

// SVG document surface: size nothing (is_svg itself stays in the render
// seam, which answers it false for every buffer).

// ra8_log backend entry: sink every line.
export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
}

// ra8_imgdec fabric: the container and pixel-layout readers are out of
// scope for this unit harness. The backend adapter reaches them only on
// the probe / decode paths, which the stubbed stb_image above refuses
// before either is consulted, so these exist only so the export
// (ra8_img_imgdec_bind) links here.
export fn ra8_imgdec_dims(bytes: [*]const u8, byte_count: u32, out: *imgdec.Ra8ImgdecGeom) callconv(.c) c_int {
    _ = bytes;
    _ = byte_count;
    _ = out;
    return k_ra8_err_not_supported;
}

export fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32 {
    _ = pixel;
    return 0;
}
