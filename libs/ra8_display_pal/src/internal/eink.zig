//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-testable half of the e-ink (IT8951) panel backend: the RGB565 to
//! greyscale conversion, the waveform mapping, the config validation order and
//! the bounded row packing. Nothing here touches the panel; the boundary in
//! `ra8_display_pal_eink_abi.zig` owns every `ra8_epaper_*` call.

const std = @import("std");
pub const core = @import("root.zig");

/// Numeric constants for the e-ink backend.
pub const eink = struct {
    pub const rgb565_bpp: u32 = 2;
    /// Typical GC16 latency, reported through caps.
    pub const refresh_quality_us: u32 = 450_000;
    /// Widest row the bounded scratch line can hold.
    pub const line_max_px: u32 = 4096;
    /// Rec.601 weights, scaled by 256.
    pub const luma_r: u32 = 77;
    pub const luma_g: u32 = 150;
    pub const luma_b: u32 = 29;
    pub const luma_shift: u5 = 8;
    pub const nibble_shift: u3 = 4;
    pub const px_per_byte_4bpp: u32 = 2;
    pub const nibble_mask: u8 = 0x0F;
    /// 4 bpp white, used to pad an odd trailing pixel.
    pub const white_nibble: u8 = 0x0F;

    pub const r5_shift: u4 = 11;
    pub const g6_shift: u4 = 5;
    pub const r5_mask: u16 = 0x1F;
    pub const g6_mask: u16 = 0x3F;
    pub const b5_mask: u16 = 0x1F;
};

/// IT8951 waveform modes, as `ra8_epaper` numbers them.
pub const Waveform = enum(u32) {
    init = 0,
    du = 1,
    gc16 = 2,
    a2 = 3,
};

/// IT8951 pixel formats, as `ra8_epaper` numbers them.
pub const PixelFormat = enum(u32) {
    bpp1 = 0,
    bpp2 = 1,
    bpp4 = 2,
    bpp8 = 3,
};

/// Backend context attached to the PAL handle.
pub const Ctx = extern struct {
    caps: core.Caps = .{},
    fb: core.Fb = .{},
    initialised: bool = false,
};

comptime {
    // `Fb` leads with a pointer, so the caps snapshot is padded out to pointer
    // alignment ahead of it.
    std.debug.assert(@offsetOf(Ctx, "caps") == 0);
    std.debug.assert(@offsetOf(Ctx, "fb") == std.mem.alignForward(usize, @sizeOf(core.Caps), @alignOf(core.Fb)));
    std.debug.assert(@offsetOf(Ctx, "initialised") == @offsetOf(Ctx, "fb") + @sizeOf(core.Fb));
}

/// Rec.601 luma of one RGB565 pixel, 8 bits.
///
/// The 5- and 6-bit channels are expanded to 8 bits by bit replication, so 0
/// stays 0 and a saturated channel stays 255.
pub fn lumaFromRgb565(px: u16) u8 {
    const r5: u32 = (px >> eink.r5_shift) & eink.r5_mask;
    const g6: u32 = (px >> eink.g6_shift) & eink.g6_mask;
    const b5: u32 = px & eink.b5_mask;
    const r8 = (r5 << 3) | (r5 >> 2);
    const g8 = (g6 << 2) | (g6 >> 4);
    const b8 = (b5 << 3) | (b5 >> 2);
    const luma = ((r8 * eink.luma_r) + (g8 * eink.luma_g) + (b8 * eink.luma_b)) >> eink.luma_shift;
    return @truncate(luma);
}

/// Waveform for a PAL refresh hint: fast picks A2, init picks INIT, anything
/// else picks GC16.
pub fn waveformFor(hint: u8) Waveform {
    if (hint == core.refresh_fast) return .a2;
    if (hint == core.refresh_init) return .init;
    return .gc16;
}

/// One packed RGB565 row.
pub fn strideBytes(width_px: u16) u32 {
    return @as(u32, width_px) * eink.rgb565_bpp;
}

/// The smallest framebuffer this geometry can be painted into. Wide for the
/// same reason as the LCD backend: a geometry no `u32` byte count could satisfy
/// must be rejected, not wrapped into one that looks satisfiable.
pub fn neededBytes(width_px: u16, height_px: u16) u64 {
    return @as(u64, strideBytes(width_px)) * @as(u64, height_px);
}

/// Bytes a packed 4 bpp row occupies, the odd-pixel tail included.
pub fn packedRowBytes(width_px: u16) u32 {
    return (@as(u32, width_px) + eink.px_per_byte_4bpp - 1) / eink.px_per_byte_4bpp;
}

/// `internal_eink_validate_cfg`: the LCD backend's rules plus the two this
/// panel adds, the BSP-supplied IT8951 descriptor and the bounded row width.
pub fn validateCfg(cfg: core.CfgView) u16 {
    if (!cfg.has_framebuffer) return core.err_null_ptr;
    if (cfg.width_px == 0 or cfg.height_px == 0) return core.err_invalid_arg;
    if (cfg.pixfmt != core.pixfmt_rgb565) return core.err_not_supported;
    if (cfg.width_px > eink.line_max_px) return core.err_invalid_arg;
    // The IT8951 descriptor is BSP-supplied through `panel_timing`.
    if (!cfg.has_panel_timing) return core.err_invalid_arg;
    if (@as(u64, cfg.framebuffer_bytes) < neededBytes(cfg.width_px, cfg.height_px)) {
        return core.err_invalid_arg;
    }
    return core.err_ok;
}

/// The caps half of `internal_eink_snapshot`. E-paper holds its image without
/// scanning, so it reports partial update and no continuous refresh, and it
/// carries the known GC16 latency.
pub fn capsFor(width_px: u16, height_px: u16) core.Caps {
    return .{
        .width_px = width_px,
        .height_px = height_px,
        .pixfmt = core.pixfmt_rgb565,
        .stride_bytes = strideBytes(width_px),
        .refresh_latency_us_typ = eink.refresh_quality_us,
        .supports_partial_update = true,
        .continuous_refresh = false,
    };
}

/// The framebuffer half of `internal_eink_snapshot`: the app keeps painting
/// canonical RGB565, and flush converts it on the way to the panel.
pub fn fbFor(pixels: ?*anyopaque, width_px: u16, height_px: u16) core.Fb {
    return .{
        .pixels = pixels,
        .width_px = width_px,
        .height_px = height_px,
        .stride_bytes = strideBytes(width_px),
        .pixfmt = core.pixfmt_rgb565,
    };
}

/// `internal_eink_check_rect`: four sequential checks, so each way out of the
/// decision is reachable on its own and both sums are 32-bit.
pub fn checkRect(caps: core.Caps, r: core.Rect) u16 {
    if (r.x > caps.width_px) return core.err_invalid_arg;
    if (r.y > caps.height_px) return core.err_invalid_arg;
    if (@as(u32, r.x) + @as(u32, r.w) > @as(u32, caps.width_px)) return core.err_invalid_arg;
    if (@as(u32, r.y) + @as(u32, r.h) > @as(u32, caps.height_px)) return core.err_invalid_arg;
    return core.err_ok;
}

/// Pack one RGB565 span into `dst` as 4 bpp greyscale, two pixels per byte,
/// the first pixel in the high nibble.
///
/// The controller keeps only the high nibble of an 8 bpp byte, so dropping the
/// low nibble costs nothing optically and halves the bytes on the wire. An odd
/// trailing pixel is paired with white so the row still ends on a byte
/// boundary, which is what the controller's row stride assumes.
pub fn packRow4bpp(src: []const u16, dst: []u8) void {
    std.debug.assert(dst.len >= packedRowBytes(@intCast(src.len)));
    var col: usize = 0;
    while (col < src.len) : (col += eink.px_per_byte_4bpp) {
        const hi_px = lumaFromRgb565(src[col]);
        const next = col + 1;
        const lo_px: u8 = if (next < src.len)
            lumaFromRgb565(src[next])
        else
            eink.white_nibble << eink.nibble_shift;
        const hi_n = (hi_px >> eink.nibble_shift) & eink.nibble_mask;
        const lo_n = (lo_px >> eink.nibble_shift) & eink.nibble_mask;
        dst[col / eink.px_per_byte_4bpp] = (hi_n << eink.nibble_shift) | lo_n;
    }
}

/// Where one row of `rect` starts, in pixels from the framebuffer origin.
pub fn rowOffsetPx(fb_width_px: u16, rect: core.Rect, row: u16) u32 {
    return ((@as(u32, rect.y) + @as(u32, row)) * @as(u32, fb_width_px)) + @as(u32, rect.x);
}

/// Total pixels in the bound framebuffer, which is what `clear` fills.
pub fn pixelCount(fb: core.Fb) u32 {
    return @as(u32, fb.width_px) * @as(u32, fb.height_px);
}

/// The context a successful deinit leaves behind.
pub fn released(ctx: Ctx) Ctx {
    var out = ctx;
    out.initialised = false;
    out.fb.pixels = null;
    return out;
}
