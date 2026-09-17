//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Fixed in-memory PCM replay backend. Binds one immutable caller-owned
//! frame and copies it into each supplied output buffer, with no allocation
//! and no hardware. Streaming is deliberately absent, so the facade answers
//! `not_supported` for this backend.

const std = @import("std");
pub const abi = @import("ra8_audio_abi.zig");
pub const core = abi.core;

/// `ra8_audio_source_memory_state_t`: the borrowed fixture, nothing else.
pub const MemoryState = extern struct {
    frame: abi.Frame = .{},
};

comptime {
    std.debug.assert(@sizeOf(MemoryState) == @sizeOf(abi.Frame));
    std.debug.assert(@offsetOf(MemoryState, "frame") == 0);
}

fn stateOf(ctx: *anyopaque) *const MemoryState {
    return @ptrCast(@alignCast(ctx));
}

fn memoryGetInfo(ctx: ?*anyopaque, out_info: ?*abi.Info) callconv(.c) u16 {
    const context = ctx orelse return abi.err.null_ptr;
    const out = out_info orelse return abi.err.null_ptr;
    const state = stateOf(context);
    out.* = .{
        .frame_bytes = state.frame.bytes,
        .samples_per_frame = state.frame.sample_count,
        .sample_rate_hz = state.frame.sample_rate_hz,
        .channels = state.frame.channels,
        .valid_bits = state.frame.valid_bits,
        .format = state.frame.format,
    };
    return abi.err.ok;
}

fn memoryCapture(
    ctx: ?*anyopaque,
    buffer: ?*const abi.Buffer,
    out_frame: ?*abi.Frame,
) callconv(.c) u16 {
    const context = ctx orelse return abi.err.null_ptr;
    const span = buffer orelse return abi.err.null_ptr;
    const out = out_frame orelse return abi.err.null_ptr;
    const state = stateOf(context);
    if (span.capacity < state.frame.bytes) return abi.err.invalid_size;
    // Both pointers are non-NULL for any state that came out of
    // `ra8_audio_source_memory_init`; answering `null_ptr` instead of
    // dereferencing is the hardening for a hand-built state.
    const destination: [*]u8 = @ptrCast(span.data orelse return abi.err.null_ptr);
    const origin: [*]const u8 = @ptrCast(state.frame.data orelse return abi.err.null_ptr);
    const length: usize = state.frame.bytes;
    @memcpy(destination[0..length], origin[0..length]);
    out.* = state.frame;
    out.data = span.data;
    return abi.err.ok;
}

fn memoryStop(ctx: ?*anyopaque) callconv(.c) u16 {
    if (ctx == null) return abi.err.null_ptr;
    return abi.err.ok;
}

/// `s_memory_source_iface`: no `stream_start` row, by design.
const memory_iface: abi.SourceIface = .{
    .get_info = memoryGetInfo,
    .capture = memoryCapture,
    .stream_start = null,
    .stop = memoryStop,
};

pub export fn ra8_audio_source_memory_init(
    source: ?*abi.Source,
    state: ?*MemoryState,
    frame: ?*const abi.Frame,
) u16 {
    const handle = source orelse return abi.err.null_ptr;
    const backing = state orelse return abi.err.null_ptr;
    const fixture = frame orelse return abi.err.null_ptr;
    const fault = core.validateFrame(fixture);
    if (fault != .ok) return abi.frameErr(fault);
    backing.frame = fixture.*;
    handle.* = .{ .iface = &memory_iface, .ctx = @ptrCast(backing) };
    return abi.err.ok;
}
