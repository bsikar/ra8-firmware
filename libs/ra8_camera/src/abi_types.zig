//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared C ABI types and policy helpers. Kept free of exported symbols so
//! each backend can be a separate archive member without duplicating the
//! facade's C ABI definitions.

const std = @import("std");
pub const core = @import("internal/root.zig");

pub const Buffer = core.Buffer;
pub const Frame = core.Frame;
pub const Info = core.Info;
pub const format = core.format;

/// `ra8_err_t` values this library can produce. Everything else a backend
/// returns is forwarded untouched.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_size: u16 = 0x105;
    pub const not_supported: u16 = 0x107;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
};

pub const GetInfoFn = *const fn (?*anyopaque, ?*Info) callconv(.c) u16;
pub const CaptureFn = *const fn (?*anyopaque, ?*const Buffer, ?*Frame) callconv(.c) u16;
pub const EncodeFn = *const fn (?*anyopaque, ?*const Frame, ?*const Buffer, ?*Frame) callconv(.c) u16;

/// `struct ra8_camera_source_iface`.
pub const SourceIface = extern struct {
    get_info: ?GetInfoFn = null,
    capture: ?CaptureFn = null,
};

/// `struct ra8_camera_codec_iface`.
pub const CodecIface = extern struct {
    encode: ?EncodeFn = null,
};

/// `ra8_camera_source_t`: caller-owned handle binding a source backend.
pub const Source = extern struct {
    iface: ?*const SourceIface = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_camera_codec_t`: caller-owned handle binding a codec backend.
pub const Codec = extern struct {
    iface: ?*const CodecIface = null,
    ctx: ?*anyopaque = null,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    std.debug.assert(@sizeOf(SourceIface) == ptr_bytes * 2);
    std.debug.assert(@offsetOf(SourceIface, "get_info") == 0);
    std.debug.assert(@offsetOf(SourceIface, "capture") == ptr_bytes);
    std.debug.assert(@sizeOf(CodecIface) == ptr_bytes);
    std.debug.assert(@offsetOf(CodecIface, "encode") == 0);
    std.debug.assert(@sizeOf(Source) == ptr_bytes * 2);
    std.debug.assert(@offsetOf(Source, "ctx") == ptr_bytes);
    std.debug.assert(@sizeOf(Codec) == ptr_bytes * 2);
    std.debug.assert(@offsetOf(Codec, "ctx") == ptr_bytes);
}

/// Frame-validation faults to `ra8_err_t`.
pub fn frameErr(fault: core.FrameFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .null_data => err.null_ptr,
        .zero_width, .zero_height, .unsupported_format, .odd_uyvy_width => err.invalid_arg,
        .jpeg_zero_bytes,
        .jpeg_stride_set,
        .stride_short,
        .height_overflows_stride,
        .bytes_short,
        => err.invalid_size,
    };
}

/// Capture-buffer faults to `ra8_err_t`.
pub fn bufferErr(fault: core.BufferFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .null_data => err.null_ptr,
        .zero_capacity => err.invalid_size,
    };
}

/// Backend contract violations to `ra8_err_t`.
pub fn captureErr(fault: core.CaptureFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .alias_mismatch, .bytes_exceed_capacity => err.invalid_size,
    };
}

/// Validate a source handle and both of its mandatory vtable rows.
pub fn sourceHandleErr(source: ?*const Source) u16 {
    const handle = source orelse return err.null_ptr;
    const iface = handle.iface orelse return err.not_initialized;
    if (iface.get_info == null or iface.capture == null) return err.not_initialized;
    return err.ok;
}
