//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_imgdec` backend's own decisions, with no ABI and no codec call in
//! sight: the frame-size ceiling and whether a caller's destination can take
//! the surface. Both are pure functions of numbers the membrane has already
//! read, which is what makes every branch host-testable without a JPEG.

/// Numbers this backend publishes about itself.
pub const limits = struct {
    /// Bytes per pixel `ra8_jpeg_sw_decode` writes (packed RGB888).
    pub const bytes_per_pixel: u32 = 3;
    /// Widest and tallest frame admitted, the `ra8_imgdec` fabric's ceiling.
    pub const dim_max: u32 = 16384;
};

comptime {
    // The destination size is computed in 32 bits, so the largest frame this
    // backend admits must fit there with its three bytes per pixel.
    const widest: u64 = limits.dim_max;
    if (widest * widest * limits.bytes_per_pixel > @as(u64, 0xFFFF_FFFF)) {
        @compileError("a dim_max frame must not overflow a 32-bit byte count");
    }
}

/// Why a destination was refused, or that it was accepted.
pub const Destination = enum {
    /// Packed and large enough.
    ok,
    /// Too small for the surface, or a stride under one packed row.
    too_small,
    /// A stride wider than one packed row: the codec cannot pad.
    padded,
};

/// Bytes one packed row of a `width`-pixel frame occupies.
pub fn rowStride(width: u16) u32 {
    return @as(u32, width) * limits.bytes_per_pixel;
}

/// Bytes the whole packed surface of a `width` x `height` frame occupies.
pub fn surfaceBytes(width: u16, height: u16) u32 {
    return rowStride(width) * @as(u32, height);
}

/// Whether a declared frame is inside the advertised `dim_max`.
pub fn withinDimMax(width: u16, height: u16) bool {
    return @as(u32, width) <= limits.dim_max and @as(u32, height) <= limits.dim_max;
}

/// Decide whether a request's destination can take the surface.
///
/// Two refusals, and they are different failures. A stride narrower than one
/// packed row is a destination too small to describe the surface at all. A
/// stride *wider* than one packed row is a padded destination, and the codec
/// has no stride parameter: it writes rows back to back. Honouring that is
/// impossible, so it is refused rather than silently ignored, which would
/// leave a caller's padded surface holding a sheared image.
///
/// A `dst_stride` of zero means tightly packed and asks nothing of the codec.
pub fn destination(dst_stride: u32, dst_bytes: u32, stride: u32, need: u32) Destination {
    if (dst_stride != 0) {
        if (dst_stride < stride) return .too_small;
        if (dst_stride > stride) return .padded;
    }
    if (dst_bytes < need) return .too_small;
    return .ok;
}
