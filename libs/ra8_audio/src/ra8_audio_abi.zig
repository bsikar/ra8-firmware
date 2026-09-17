//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the transport-neutral audio facade. Exports the five
//! `ra8_audio_*` symbols behind the unchanged `inc/ra8_audio.h`, mirrors the
//! private backend vtable from `src/ra8_audio_internal.h` (which stays: the
//! host suite includes it to build its own fake backend), and translates the
//! pure policy in `internal/root.zig` into `ra8_err_t` values.

const std = @import("std");
pub const core = @import("internal/root.zig");

pub const Frame = core.Frame;
pub const Info = core.Info;
pub const Buffer = core.Buffer;

/// `ra8_err_t` values this library can produce or forward.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const not_supported: u16 = 0x107;
    pub const exists: u16 = 0x10C;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_timeout: u16 = 0x203;
    pub const null_ptr: u16 = 0x504;
};

/// `ra8_audio_frame_callback_t`.
pub const FrameCallback = *const fn (?*anyopaque, ?*const Frame) callconv(.c) void;

pub const GetInfoFn = *const fn (?*anyopaque, ?*Info) callconv(.c) u16;
pub const CaptureFn = *const fn (?*anyopaque, ?*const Buffer, ?*Frame) callconv(.c) u16;
pub const StreamStartFn = *const fn (?*anyopaque, ?*const Buffer, ?FrameCallback, ?*anyopaque) callconv(.c) u16;
pub const StopFn = *const fn (?*anyopaque) callconv(.c) u16;

/// `struct ra8_audio_source_iface`: the private backend vtable. Every row is
/// optional because the facade's contract is written in terms of absent rows
/// (no `stream_start` is `not_supported`, no `capture` is `not_initialized`).
pub const SourceIface = extern struct {
    get_info: ?GetInfoFn = null,
    capture: ?CaptureFn = null,
    stream_start: ?StreamStartFn = null,
    stop: ?StopFn = null,
};

/// `ra8_audio_source_t`: caller-owned handle binding a backend to its state.
pub const Source = extern struct {
    iface: ?*const SourceIface = null,
    ctx: ?*anyopaque = null,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    std.debug.assert(@sizeOf(SourceIface) == ptr_bytes * 4);
    std.debug.assert(@offsetOf(SourceIface, "get_info") == 0);
    std.debug.assert(@offsetOf(SourceIface, "capture") == ptr_bytes);
    std.debug.assert(@offsetOf(SourceIface, "stream_start") == ptr_bytes * 2);
    std.debug.assert(@offsetOf(SourceIface, "stop") == ptr_bytes * 3);
    std.debug.assert(@sizeOf(Source) == ptr_bytes * 2);
    std.debug.assert(@offsetOf(Source, "iface") == 0);
    std.debug.assert(@offsetOf(Source, "ctx") == ptr_bytes);
}

/// Frame-validation faults to `ra8_err_t`, matching the C exactly: an absent
/// sample pointer is `null_ptr`, bad scalar metadata is `invalid_arg`, and a
/// byte count that disagrees with the geometry is `invalid_size`.
pub fn frameErr(fault: core.FrameFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        .null_data => err.null_ptr,
        .bad_format,
        .zero_sample_count,
        .zero_sample_rate,
        .zero_channels,
        .zero_valid_bits,
        .valid_bits_too_wide,
        => err.invalid_arg,
        .size_overflow, .bytes_mismatch => err.invalid_size,
    };
}

/// Address of an optional pointer, 0 when absent. Used to compare a returned
/// frame's payload against the caller's buffer without unwrapping either.
fn addressOf(pointer: ?*const anyopaque) usize {
    return if (pointer) |value| @intFromPtr(value) else 0;
}

pub export fn ra8_audio_frame_validate(frame: ?*const Frame) u16 {
    const view = frame orelse return err.null_ptr;
    return frameErr(core.validateFrame(view));
}

pub export fn ra8_audio_source_get_info(source: ?*Source, out_info: ?*Info) u16 {
    const handle = source orelse return err.null_ptr;
    const out = out_info orelse return err.null_ptr;
    out.* = .{};
    const iface = handle.iface orelse return err.not_initialized;
    const get_info = iface.get_info orelse return err.not_initialized;
    return get_info(handle.ctx, out);
}

pub export fn ra8_audio_source_capture(
    source: ?*Source,
    buffer: ?*const Buffer,
    out_frame: ?*Frame,
) u16 {
    const handle = source orelse return err.null_ptr;
    const span = buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    out.* = .{};
    const iface = handle.iface orelse return err.not_initialized;
    const capture = iface.capture orelse return err.not_initialized;
    // A zero-capacity or data-less buffer is an absent buffer, not a size error.
    switch (core.bufferFault(span)) {
        .ok => {},
        .null_data, .zero_capacity => return err.null_ptr,
    }
    // Hardening, unreachable through any `_init()` helper in this repo: the C
    // called `get_info` here without checking the row, so a hand-built vtable
    // carrying `capture` but no `get_info` dereferenced NULL.
    const get_info = iface.get_info orelse return err.not_initialized;

    var info: Info = .{};
    var status = get_info(handle.ctx, &info);
    if (status != err.ok) return status;
    if (!core.capacityFits(span.capacity, info.frame_bytes)) return err.invalid_size;

    status = capture(handle.ctx, span, out);
    if (status != err.ok) {
        out.* = .{};
        return status;
    }
    if (addressOf(out.data) != addressOf(span.data)) {
        out.* = .{};
        return err.invalid_state;
    }
    const fault = core.validateFrame(out);
    if (fault != .ok) {
        out.* = .{};
        return frameErr(fault);
    }
    if (core.frameMatchesInfo(out, &info) != .ok) {
        out.* = .{};
        return err.invalid_state;
    }
    return err.ok;
}

pub export fn ra8_audio_source_stream_start(
    source: ?*Source,
    buffer: ?*const Buffer,
    callback: ?FrameCallback,
    ctx: ?*anyopaque,
) u16 {
    const handle = source orelse return err.null_ptr;
    const span = buffer orelse return err.null_ptr;
    const sink = callback orelse return err.null_ptr;
    if (span.data == null) return err.null_ptr;
    const iface = handle.iface orelse return err.not_initialized;
    const get_info = iface.get_info orelse return err.not_initialized;
    const stream_start = iface.stream_start orelse return err.not_supported;

    var info: Info = .{};
    const status = get_info(handle.ctx, &info);
    if (status != err.ok) return status;
    if (!core.capacityFits(span.capacity, info.frame_bytes)) return err.invalid_size;
    return stream_start(handle.ctx, span, sink, ctx);
}

pub export fn ra8_audio_source_stop(source: ?*Source) u16 {
    const handle = source orelse return err.null_ptr;
    const iface = handle.iface orelse return err.not_initialized;
    const stop = iface.stop orelse return err.not_initialized;
    const status = stop(handle.ctx);
    if (status == err.ok) handle.* = .{};
    return status;
}
