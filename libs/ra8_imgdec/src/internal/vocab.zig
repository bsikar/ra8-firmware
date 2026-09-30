//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The vocabulary every other file in `ra8_imgdec` speaks: the error codes the
//! C ABI publishes, the container and pixel bit sets, the bounds the fabric
//! enforces, and the two predicates that read them.
//!
//! Constants are `pub const` under a namespace struct rather than a flat
//! `k_`-prefixed enum, so a reader sees `Format.webp` and `Pixel.rgba8888`
//! instead of two prefixes that only differ in the middle.

/// `ra8_err_t` values this library returns, from `ra8_err.h`.
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const no_mem: u16 = 0x102;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const invalid_size: u16 = 0x105;
    pub const not_found: u16 = 0x106;
    pub const not_supported: u16 = 0x107;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
};

/// `ra8_imgdec_format_t`: the container a buffer opens with, as a bit.
pub const Format = struct {
    pub const none: u32 = 0;
    pub const jpeg: u32 = 1 << 0;
    pub const png: u32 = 1 << 1;
    pub const webp: u32 = 1 << 2;
    pub const gif: u32 = 1 << 3;
    pub const bmp: u32 = 1 << 4;
    pub const tga: u32 = 1 << 5;
    /// Every defined bit. Matches `k_ra8_imgdec_format_mask`.
    pub const mask: u32 = 0x3F;
};

/// `ra8_imgdec_pixel_t`: the destination layout a decode writes, as a bit.
pub const Pixel = struct {
    pub const none: u32 = 0;
    pub const grey8: u32 = 1 << 0;
    pub const rgb888: u32 = 1 << 1;
    pub const rgba8888: u32 = 1 << 2;
    /// Every defined bit. Matches `k_ra8_imgdec_pixel_mask`.
    pub const mask: u32 = 0x07;
};

/// `ra8_imgdec_limits_t`: bounds the fabric enforces before a backend runs.
pub const Limits = struct {
    pub const dim_max: u32 = 16384;
    pub const sniff_bytes: u32 = 12;
    pub const dims_bytes: u32 = 30;
};

/// Closed error set for the pure header readers. The fabric and the mux do not
/// use it: they hand a backend's own `ra8_err_t` back untranslated, and a set
/// of named cases cannot carry a code this library never defined.
pub const Fault = error{
    InvalidSize,
    NotFound,
    NotSupported,
};

/// The `ra8_err_t` a `Fault` crosses the membrane as.
pub fn faultCode(fault: Fault) u16 {
    return switch (fault) {
        Fault.InvalidSize => Err.invalid_size,
        Fault.NotFound => Err.not_found,
        Fault.NotSupported => Err.not_supported,
    };
}

/// Bytes one pixel of `pixel` occupies, 0 unless it is exactly one defined bit.
pub fn pixelBytes(pixel: u32) u32 {
    return switch (pixel) {
        Pixel.grey8 => 1,
        Pixel.rgb888 => 3,
        Pixel.rgba8888 => 4,
        else => 0,
    };
}

/// True when `mask` is exactly one bit and that bit is in `defined`.
pub fn oneDefinedBit(mask: u32, defined: u32) bool {
    const single = (mask != 0) and ((mask & (mask -% 1)) == 0);
    return single and ((mask & defined) != 0);
}
