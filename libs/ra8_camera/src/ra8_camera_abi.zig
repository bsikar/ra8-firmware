//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the camera facade. Exports the four `ra8_camera_*`
//! symbols behind the unchanged `inc/ra8_camera.h`, mirrors the private source
//! and codec vtables from `src/ra8_camera_internal.h` (which stays: four host
//! suites include it to build their own fake backends, and the CEU backend is
//! still a C translation unit), and translates the pure policy in
//! `internal/root.zig` into `ra8_err_t` values.

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

/// `struct ra8_camera_source_iface`. Both rows are optional: the facade's
/// contract is written in terms of an absent row being `not_initialized`.
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

/// Frame-validation faults to `ra8_err_t`, matching the C exactly: an absent
/// pointer is `null_ptr`, bad scalar metadata or an unusable format is
/// `invalid_arg`, and storage that cannot cover the geometry is `invalid_size`.
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

/// Backend contract violations to `ra8_err_t`. Both are `invalid_size` in the C.
pub fn captureErr(fault: core.CaptureFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .alias_mismatch, .bytes_exceed_capacity => err.invalid_size,
    };
}

/// Validate a source handle and both of its mandatory vtable rows.
///
/// A NULL handle is `null_ptr`; a handle whose vtable or either mandatory row
/// is absent is `not_initialized`.
pub fn sourceHandleErr(source: ?*const Source) u16 {
    const handle = source orelse return err.null_ptr;
    const iface = handle.iface orelse return err.not_initialized;
    if (iface.get_info == null) {
        return err.not_initialized;
    }
    if (iface.capture == null) {
        return err.not_initialized;
    }
    return err.ok;
}

pub export fn ra8_camera_frame_validate(frame: ?*const Frame) callconv(.c) u16 {
    const view = frame orelse return err.null_ptr;
    return frameErr(core.validateFrame(view.*));
}

pub export fn ra8_camera_source_get_info(source: ?*Source, out_info: ?*Info) callconv(.c) u16 {
    const info = out_info orelse return err.null_ptr;
    info.* = .{};
    const handle = sourceHandleErr(source);
    if (handle != err.ok) {
        return handle;
    }
    const bound = source.?;
    return bound.iface.?.get_info.?(bound.ctx, info);
}

pub export fn ra8_camera_source_capture(
    source: ?*Source,
    buffer: ?*const Buffer,
    out_frame: ?*Frame,
) callconv(.c) u16 {
    const span = buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    out.* = .{};
    const handle = sourceHandleErr(source);
    if (handle != err.ok) {
        return handle;
    }
    const buffer_fault = core.captureBufferFault(span.data != null, span.capacity);
    if (buffer_fault != .ok) {
        return bufferErr(buffer_fault);
    }
    const bound = source.?;
    const captured = bound.iface.?.capture.?(bound.ctx, span, out);
    if (captured != err.ok) {
        out.* = .{};
        return captured;
    }
    const post = core.capturePostFault(
        @intFromPtr(out.data),
        @intFromPtr(span.data),
        out.bytes,
        span.capacity,
    );
    if (post != .ok) {
        out.* = .{};
        return captureErr(post);
    }
    const valid = frameErr(core.validateFrame(out.*));
    if (valid != err.ok) {
        out.* = .{};
    }
    return valid;
}

pub export fn ra8_camera_codec_encode(
    codec: ?*Codec,
    input: ?*const Frame,
    output_buffer: ?*const Buffer,
    out_frame: ?*Frame,
) callconv(.c) u16 {
    const bound = codec orelse return err.null_ptr;
    const source_frame = input orelse return err.null_ptr;
    const output = output_buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    out.* = .{};
    const iface = bound.iface orelse return err.not_initialized;
    const encode = iface.encode orelse return err.not_initialized;
    const valid = frameErr(core.validateFrame(source_frame.*));
    if (valid != err.ok) {
        return valid;
    }
    const encoded = encode(bound.ctx, source_frame, output, out);
    if (encoded != err.ok) {
        out.* = .{};
        return encoded;
    }
    const output_valid = frameErr(core.validateFrame(out.*));
    if (output_valid != err.ok) {
        out.* = .{};
    }
    return output_valid;
}
