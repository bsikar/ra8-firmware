//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Fixed-frame memory source backend: bounded metadata replay and a
//! byte-for-byte copy into caller-owned capture storage.

const std = @import("std");
pub const abi = @import("abi_types.zig");

const core = abi.core;
const err = abi.err;

/// `ra8_camera_source_memory_state_t`: the borrowed fixed frame.
pub const State = extern struct {
    frame: core.Frame = .{},
};

comptime {
    std.debug.assert(@sizeOf(State) == @sizeOf(core.Frame));
    std.debug.assert(@offsetOf(State, "frame") == 0);
}

/// Source vtable row: report the fixed frame's native geometry.
fn getInfo(ctx: ?*anyopaque, out_info: ?*core.Info) callconv(.c) u16 {
    const raw = ctx orelse return err.null_ptr;
    const info = out_info orelse return err.null_ptr;
    const state: *const State = @ptrCast(@alignCast(raw));
    info.* = .{
        .frame_bytes_max = state.frame.bytes,
        .stride_bytes = state.frame.stride_bytes,
        .width = state.frame.width,
        .height = state.frame.height,
        .format = state.frame.format,
    };
    return err.ok;
}

/// Source vtable row: copy the fixed frame into the caller's capture buffer.
fn capture(ctx: ?*anyopaque, buffer: ?*const core.Buffer, out_frame: ?*core.Frame) callconv(.c) u16 {
    const raw = ctx orelse return err.null_ptr;
    const span = buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    const state: *const State = @ptrCast(@alignCast(raw));
    if (!core.memoryCapacityFits(span.capacity, state.frame.bytes)) {
        return err.invalid_size;
    }
    const destination = span.data.?;
    const source_bytes = state.frame.data.?;
    @memcpy(destination[0..state.frame.bytes], source_bytes[0..state.frame.bytes]);
    out.* = state.frame;
    out.data = destination;
    return err.ok;
}

/// Fixed-frame source vtable.
pub const iface: abi.SourceIface = .{ .get_info = getInfo, .capture = capture };

pub export fn ra8_camera_source_memory_init(
    source: ?*abi.Source,
    state: ?*State,
    frame: ?*const core.Frame,
) callconv(.c) u16 {
    const handle = source orelse return err.null_ptr;
    const backend = state orelse return err.null_ptr;
    const fixed = frame orelse return err.null_ptr;
    const valid = abi.frameErr(core.validateFrame(fixed.*));
    if (valid != err.ok) {
        return valid;
    }
    backend.frame = fixed.*;
    handle.iface = &iface;
    handle.ctx = backend;
    return err.ok;
}
