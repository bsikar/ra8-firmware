//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The lifecycle half of `ra8_gfx`: turning a bind request into either an
//! error code or a fully populated framebuffer binding, and collapsing that
//! binding again on teardown. Pure in both directions, so the module state
//! object itself is only ever assigned at the membrane in
//! `../ra8_gfx_abi.zig`.
//!
//! Two bind forms exist. The positional one assumes a densely packed buffer,
//! which is what every caller before #737 had; the surface one carries the row
//! pitch, so a backend that padded its rows hands the binding over intact.
//! Both produce the same `core.State`, and a packed surface is bit-for-bit
//! what the positional form gives.

const std = @import("std");

/// The pure core, re-exported so a test module reaching this file also reaches
/// the error codes and the state layout without importing `root.zig` twice.
pub const core = @import("root.zig");

/// `ra8_gfx_surface_t` from `inc/ra8_gfx.h`: the four positional values plus
/// the row pitch. Read through a caller pointer, so the layout is ABI.
pub const Surface = extern struct {
    pixels: ?*anyopaque = null,
    w: u16 = 0,
    h: u16 = 0,
    stride_bytes: u32 = 0,
    fmt: u8 = core.format.rgb565,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@offsetOf(Surface, "pixels") == 0);
    std.debug.assert(@offsetOf(Surface, "w") == ptr);
    std.debug.assert(@offsetOf(Surface, "h") == ptr + 2);
    std.debug.assert(@offsetOf(Surface, "stride_bytes") == ptr + 4);
    std.debug.assert(@offsetOf(Surface, "fmt") == ptr + 8);
}

/// One densely packed row of `w` pixels in `fmt`, in bytes.
pub fn packedRow(w: u16, fmt: u8) u32 {
    return @as(u32, w) * @as(u32, core.bppOf(fmt));
}

/// Whether an edge length is one this library binds.
pub fn dimOk(v: u16) bool {
    return (v >= core.dim.min) and (v <= core.dim.max);
}

/// Status of a positional bind request: `ok` or the code to return.
pub fn checkDims(w: u16, h: u16, fmt: u8) u16 {
    if (!dimOk(w) or !dimOk(h)) return core.err.invalid_arg;
    if (!core.formatOk(fmt)) return core.err.invalid_arg;
    return core.err.ok;
}

/// Status of a surface bind request: the positional checks plus the pitch.
///
/// A pitch narrower than one packed row cannot describe any real buffer, since
/// row `y + 1` would start inside row `y`. A wider pitch is padding and is
/// honoured as given.
pub fn checkSurface(s: Surface) u16 {
    const status = checkDims(s.w, s.h, s.fmt);
    if (status != core.err.ok) return status;
    if (s.stride_bytes < packedRow(s.w, s.fmt)) return core.err.invalid_arg;
    return core.err.ok;
}

/// The binding a successful bind installs: clip defaults to the whole buffer.
pub fn bound(fb: [*]u8, w: u16, h: u16, fmt: u8, pitch: u32) core.State {
    return .{
        .fb = fb,
        .width = w,
        .height = h,
        .pitch = pitch,
        .format = fmt,
        .bpp = core.bppOf(fmt),
        .initialized = true,
        .clip_x0 = 0,
        .clip_y0 = 0,
        .clip_x1 = @intCast(w),
        .clip_y1 = @intCast(h),
    };
}

/// The binding teardown leaves behind. The framebuffer belongs to the caller,
/// so nothing is released beyond the binding itself: the pointer is dropped so
/// no later draw call can reach a buffer the caller has retired, and the clip
/// collapses to empty. The format is deliberately left as it was, which is
/// what the C did.
pub fn released(current: core.State) core.State {
    return .{ .format = current.format };
}
