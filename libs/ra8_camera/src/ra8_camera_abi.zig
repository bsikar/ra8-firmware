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
const types = @import("abi_types.zig");

pub const core = types.core;
pub const Buffer = types.Buffer;
pub const Frame = types.Frame;
pub const Info = types.Info;
pub const format = types.format;
pub const err = types.err;
pub const GetInfoFn = types.GetInfoFn;
pub const CaptureFn = types.CaptureFn;
pub const EncodeFn = types.EncodeFn;
pub const SourceIface = types.SourceIface;
pub const CodecIface = types.CodecIface;
pub const Source = types.Source;
pub const Codec = types.Codec;
pub const frameErr = types.frameErr;
pub const bufferErr = types.bufferErr;
pub const captureErr = types.captureErr;
pub const sourceHandleErr = types.sourceHandleErr;

const ptr_bytes = @sizeOf(usize);

comptime {
    std.debug.assert(@sizeOf(Buffer) == ptr_bytes * 2);
    std.debug.assert(@offsetOf(Frame, "stride_bytes") == ptr_bytes + 4);
    std.debug.assert(@offsetOf(Frame, "width") == ptr_bytes + 8);
    std.debug.assert(@offsetOf(Frame, "height") == ptr_bytes + 10);
    std.debug.assert(@offsetOf(Frame, "format") == ptr_bytes + 12);
    std.debug.assert(@sizeOf(Frame) == std.mem.alignForward(usize, ptr_bytes + 13, ptr_bytes));
    std.debug.assert(@sizeOf(Info) == 16);
    std.debug.assert(@sizeOf(SourceIface) == ptr_bytes * 2);
    std.debug.assert(@sizeOf(CodecIface) == ptr_bytes);
    std.debug.assert(@sizeOf(Source) == ptr_bytes * 2);
    std.debug.assert(@sizeOf(Codec) == ptr_bytes * 2);
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
