// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Test-only link seam for the reflow Zig unit test harness (RA8FW-1231).
//!
//! Answers the vendored stb_image, ra8_webp, ra8_jpeg, ra8_imgdec mux,
//! ra8_img_arena, and ra8_log externs with documented not-supported / empty
//! results so unit suites link cleanly standalone.

const k_ra8_err_not_supported: c_int = 0x107;
const k_ra8_err_not_supported_u16: u16 = 0x107;

const imgdec = @import("reflow").imgdec;

export fn ra8_jpeg_imgdec_bind(out: *imgdec.Ra8Imgdec) callconv(.c) u16 {
    out.* = .{ .iface = null, .ctx = null };
    return 0;
}

export fn ra8_webp_imgdec_bind(out: *imgdec.Ra8Imgdec, scratch: *imgdec.Ra8ImgArena) callconv(.c) u16 {
    out.* = .{ .iface = null, .ctx = scratch };
    return 0;
}

export fn ra8_imgdec_mux_init(mux: *anyopaque) callconv(.c) u16 {
    _ = mux;
    return 0;
}

export fn ra8_imgdec_mux_add(mux: *anyopaque, dec: *const imgdec.Ra8Imgdec) callconv(.c) u16 {
    _ = mux;
    _ = dec;
    return 0;
}

export fn ra8_imgdec_mux_probe(
    mux: *const anyopaque,
    bytes: [*]const u8,
    byte_count: u32,
    pixel: u32,
    out_geom: *imgdec.Ra8ImgdecGeom,
    out_member: ?*?*const imgdec.Ra8Imgdec,
) callconv(.c) u16 {
    _ = mux;
    _ = bytes;
    _ = byte_count;
    _ = pixel;
    out_geom.* = .{ .format = 0, .width_px = 0, .height_px = 0 };
    if (out_member) |slot| slot.* = null;
    return k_ra8_err_not_supported_u16;
}

export fn ra8_imgdec_mux_decode(
    mux: *const anyopaque,
    req: *const imgdec.Ra8ImgdecReq,
    out: *imgdec.Ra8ImgdecImage,
) callconv(.c) u16 {
    _ = mux;
    _ = req;
    out.* = .{
        .width_px = 0,
        .height_px = 0,
        .stride = 0,
        .used_bytes = 0,
        .format = 0,
        .pixel = 0,
        .had_alpha = false,
    };
    return k_ra8_err_not_supported_u16;
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

export fn ra8_img_arena_bind(arena: *anyopaque) callconv(.c) void {
    _ = arena;
}

export fn ra8_img_arena_unbind() callconv(.c) void {}

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
}

export fn ra8_imgdec_dims(bytes: [*]const u8, byte_count: u32, out: *imgdec.Ra8ImgdecGeom) callconv(.c) c_int {
    _ = bytes;
    _ = byte_count;
    out.* = .{ .format = 0, .width_px = 0, .height_px = 0 };
    return k_ra8_err_not_supported;
}

export fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32 {
    _ = pixel;
    return 0;
}
