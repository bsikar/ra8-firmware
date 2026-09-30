//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_jpeg/inc/ra8_jpeg_imgdec.h`: the `ra8_imgdec`
//! backend over the first-party software JPEG codec (#768). The decisions are
//! in `internal/imgdec.zig`; this file owns the mirrored seam records, the one
//! vtable instance, the guard order and the `ra8_err_t` mapping.
//!
//! The codec entry points are `extern`: they are still C in this library
//! (`ra8_jpeg_sw.c`, `ra8_jpeg_sw_decode.c`), and this backend only ever
//! reaches them through the public header's contract, never through
//! `ra8_jpeg_sw_internal.h`.

const std = @import("std");
const policy = @import("internal/imgdec.zig");

/// Subset of `ra8_err_t` this backend returns.
pub const Error = enum(u16) {
    ok = 0,
    invalid_size = 0x105,
    not_supported = 0x107,
    null_ptr = 0x504,
};

/// The `ra8_imgdec_format_t` bits this backend names.
pub const format = struct {
    pub const jpeg: u32 = 1 << 0;
};

/// The `ra8_imgdec_pixel_t` bits this backend names.
pub const pixel = struct {
    pub const rgb888: u32 = 1 << 1;
};

/// Component tag on the backend's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_jpeg_imgdec";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

extern fn ra8_jpeg_sw_get_dimensions(
    jpeg_buf: [*]const u8,
    jpeg_len: u32,
    out_w: *u16,
    out_h: *u16,
) callconv(.c) u16;

extern fn ra8_jpeg_sw_decode(
    jpeg_buf: [*]const u8,
    jpeg_len: u32,
    out_buf: [*]u8,
    out_buf_len: u32,
    out_w: *u16,
    out_h: *u16,
) callconv(.c) u16;

/// Mirror of `ra8_imgdec_caps_t`.
pub const Caps = extern struct {
    formats: u32,
    pixels: u32,
    scratch_bytes: u32,
    scratch_align: u32,
    dim_max: u32,
    streams: bool,
};

/// Mirror of `ra8_imgdec_req_t`.
pub const Request = extern struct {
    bytes: ?[*]const u8,
    byte_count: u32,
    arena: ?*anyopaque,
    dst: ?[*]u8,
    dst_bytes: u32,
    dst_stride: u32,
    format: u32,
    want: u32,
};

/// Mirror of `ra8_imgdec_image_t`.
pub const Image = extern struct {
    width_px: u32,
    height_px: u32,
    stride: u32,
    used_bytes: u32,
    format: u32,
    pixel: u32,
    had_alpha: bool,
};

/// Mirror of `struct ra8_imgdec_iface`.
pub const Interface = extern struct {
    get_caps: *const fn (ctx: ?*anyopaque, out: ?*Caps) callconv(.c) u16,
    decode: *const fn (ctx: ?*anyopaque, req: ?*const Request, out: ?*Image) callconv(.c) u16,
};

/// Mirror of `ra8_imgdec_t`.
pub const Handle = extern struct {
    iface: ?*const Interface,
    ctx: ?*anyopaque,
};

/// Report this backend's static capabilities.
///
/// Baseline JPEG into packed RGB888, no scratch, `dim_max` at the fabric
/// limit. The codec's own frame ceiling is the 16-bit SOF field, wider than
/// the seam admits, so the seam limit is the binding one and is published as
/// such rather than a number this file invents.
fn caps(ctx: ?*anyopaque, out: ?*Caps) callconv(.c) u16 {
    _ = ctx;
    const record = out orelse {
        ra8_log_emit_error(tag, "caps: null out");
        return @intFromEnum(Error.null_ptr);
    };
    record.* = .{
        .formats = format.jpeg,
        .pixels = pixel.rgb888,
        .scratch_bytes = 0,
        .scratch_align = 0,
        .dim_max = policy.limits.dim_max,
        .streams = false, // the whole frame must be resident
    };
    return @intFromEnum(Error.ok);
}

/// Read the frame's declared geometry and hold it to `dim_max`.
///
/// The pre-flight is not redundant with the decode that follows it. The fabric
/// hands a backend three duties it cannot discharge without the geometry
/// first: refuse a frame past the advertised `dim_max`, refuse a destination
/// too small for the surface, and refuse a stride the decoder cannot honour.
/// `ra8_jpeg_sw_get_dimensions` is the cheap way to get it, since it walks
/// markers and does no entropy decoding.
fn probe(req: *const Request, out_w: *u16, out_h: *u16) Error {
    const bytes = req.bytes orelse return .null_ptr;
    const err = ra8_jpeg_sw_get_dimensions(bytes, req.byte_count, out_w, out_h);
    if (err != @intFromEnum(Error.ok)) return @enumFromInt(err);
    if (!policy.withinDimMax(out_w.*, out_h.*)) return .invalid_size;
    return .ok;
}

/// Decode one baseline JPEG into the request's packed RGB888 surface.
///
/// The fabric has already proved the pointers, the non-zero counts, that
/// `want` is the one layout this backend advertised and that `format` is
/// JPEG. What is left is this module's own: read the geometry, hold it to
/// `dim_max`, prove the destination, decode, and describe what was written.
///
/// The geometry reported in `out` is the decoder's own, not the pre-flight's.
/// Both read the same SOF field so they cannot disagree, and the decoder's
/// pair is the one that describes the bytes now sitting in `dst`.
fn decode(ctx: ?*anyopaque, req: ?*const Request, out: ?*Image) callconv(.c) u16 {
    _ = ctx;
    const request = req orelse {
        ra8_log_emit_error(tag, "decode: null req");
        return @intFromEnum(Error.null_ptr);
    };
    const image = out orelse {
        ra8_log_emit_error(tag, "decode: null out");
        return @intFromEnum(Error.null_ptr);
    };

    var width: u16 = 0;
    var height: u16 = 0;
    const probed = probe(request, &width, &height);
    if (probed != .ok) return @intFromEnum(probed);

    const stride = policy.rowStride(width);
    const need = policy.surfaceBytes(width, height);
    switch (policy.destination(request.dst_stride, request.dst_bytes, stride, need)) {
        .ok => {},
        .too_small => return @intFromEnum(Error.invalid_size),
        // the codec writes packed rows only
        .padded => return @intFromEnum(Error.not_supported),
    }

    const source = request.bytes orelse return @intFromEnum(Error.null_ptr);
    const destination = request.dst orelse return @intFromEnum(Error.null_ptr);
    var decoded_w: u16 = 0;
    var decoded_h: u16 = 0;
    const err = ra8_jpeg_sw_decode(
        source,
        request.byte_count,
        destination,
        request.dst_bytes,
        &decoded_w,
        &decoded_h,
    );
    if (err != @intFromEnum(Error.ok)) return err;

    image.* = .{
        .width_px = decoded_w,
        .height_px = decoded_h,
        .stride = policy.rowStride(decoded_w),
        .used_bytes = policy.surfaceBytes(decoded_w, decoded_h),
        .format = format.jpeg,
        .pixel = pixel.rgb888,
        .had_alpha = false, // JPEG carries no alpha channel
    };
    return @intFromEnum(Error.ok);
}

/// The one vtable instance; the handle carries no state of its own.
const interface: Interface = .{ .get_caps = caps, .decode = decode };

/// Bind the software JPEG codec into an `ra8_imgdec_t` handle.
///
/// The private context is NULL deliberately: the codec's state is
/// module-static, so a per-handle context would claim an independence the
/// decoder does not have.
pub export fn ra8_jpeg_imgdec_bind(out: ?*Handle) callconv(.c) u16 {
    const handle = out orelse {
        ra8_log_emit_error(tag, "bind: null out");
        return @intFromEnum(Error.null_ptr);
    };
    handle.* = .{ .iface = &interface, .ctx = null };
    return @intFromEnum(Error.ok);
}
