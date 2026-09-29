//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure decision logic for the GLCDC/LCD backend of the display PAL: what a
//! caller's `display_cfg_t` has to satisfy, the geometry the backend snapshots
//! out of it, the bounds a flushed rectangle has to respect, and the spans a
//! cache clean has to cover. Nothing here touches MMIO, the GLCDC HAL or the
//! board; `../ra8_display_pal_lcd_abi.zig` owns the bring-up sequence, the
//! vtable and the barrier.

const std = @import("std");

/// The backend-agnostic half's types and error codes, re-exported so a module
/// that imports this file reaches them through one import.
pub const core = @import("root.zig");

/// Numeric constants of the LCD bring-up, `ra8_display_pal_lcd_const_t` and
/// `disp_lcd_mask_t` in the C.
pub const lcd = struct {
    /// Pin/clock settle after `ra8_board_glcdc_init`, in milliseconds.
    pub const settle_ms: u32 = 200;
    /// 24-bit ARGB written to the BG plane.
    pub const bg_color_black: u32 = 0;
    /// AXI burst alignment the framebuffer is expected to honour (HUM Ch 63).
    pub const fb_align_bytes: u32 = 64;
    /// Bytes per RGB565 pixel.
    pub const rgb565_bpp: u32 = 2;
    /// RGB565 pixel mask applied to `display_clear`'s 32-bit colour.
    pub const rgb565_mask: u32 = 0xFFFF;
};

/// `lcd_ctx_t`: the backend's private context, reached through the PAL
/// handle's opaque `ctx` pointer. Carries the caps and framebuffer snapshots
/// so `get_caps` and `get_framebuffer` stay O(1) reads.
pub const Ctx = extern struct {
    caps: core.Caps = .{},
    fb: core.Fb = .{},
    started: bool = false,
};

/// The fields of a `display_cfg_t` this backend judges, lifted out of the
/// pointer so the decision is testable without one.
pub const CfgView = struct {
    has_framebuffer: bool,
    width_px: u16,
    height_px: u16,
    pixfmt: u8,
    has_panel_timing: bool,
    framebuffer_bytes: u32,
};

/// One packed RGB565 row.
pub fn strideBytes(width_px: u16) u32 {
    return @as(u32, width_px) * lcd.rgb565_bpp;
}

/// The smallest framebuffer this geometry can be painted into. Widened to 64
/// bits on purpose: the C computed this in `uint32_t`, so a geometry needing
/// more than 4 GiB wrapped to a small figure and could be *accepted*. No
/// `uint32_t` byte count can satisfy such a geometry, so computing it wide
/// rejects it instead.
pub fn neededBytes(width_px: u16, height_px: u16) u64 {
    return @as(u64, strideBytes(width_px)) * @as(u64, height_px);
}

/// `internal_lcd_validate_cfg`, in the C's order: the framebuffer pointer
/// first, then the dimensions, then the pixel format, then the panel timing,
/// then the buffer size. The order is the contract, because a caller with two
/// faults sees the first one named.
pub fn validateCfg(cfg: CfgView) u16 {
    if (!cfg.has_framebuffer) return core.err_null_ptr;
    if (cfg.width_px == 0 or cfg.height_px == 0) return core.err_invalid_arg;
    if (cfg.pixfmt != core.pixfmt_rgb565) return core.err_not_supported;
    // The GLCDC path needs the panel's RGB timing; the board BSP supplies it.
    // Host and e-ink backends leave it null, which is why it is checked here
    // and not in the dispatcher.
    if (!cfg.has_panel_timing) return core.err_invalid_arg;
    if (@as(u64, cfg.framebuffer_bytes) < neededBytes(cfg.width_px, cfg.height_px)) {
        return core.err_invalid_arg;
    }
    return core.err_ok;
}

/// The caps half of `internal_lcd_snapshot`. An LCD scans continuously and
/// answers a partial update, and its refresh latency is not a figure this
/// backend reports.
pub fn capsFor(width_px: u16, height_px: u16) core.Caps {
    return .{
        .width_px = width_px,
        .height_px = height_px,
        .pixfmt = core.pixfmt_rgb565,
        .stride_bytes = strideBytes(width_px),
        .refresh_latency_us_typ = 0,
        .supports_partial_update = true,
        .continuous_refresh = true,
    };
}

/// The framebuffer half of `internal_lcd_snapshot`: the caller's own buffer,
/// described back to it.
pub fn fbFor(pixels: ?*anyopaque, width_px: u16, height_px: u16) core.Fb {
    return .{
        .pixels = pixels,
        .width_px = width_px,
        .height_px = height_px,
        .stride_bytes = strideBytes(width_px),
        .pixfmt = core.pixfmt_rgb565,
    };
}

/// `internal_lcd_check_rect`: four sequential checks rather than one compound
/// boolean, so each way out of the decision is reachable on its own. The two
/// sums are 32-bit, so a rectangle that would overflow `u16` is rejected
/// rather than wrapping into range.
pub fn checkRect(caps: core.Caps, r: core.Rect) u16 {
    if (r.x > caps.width_px) return core.err_invalid_arg;
    if (r.y > caps.height_px) return core.err_invalid_arg;
    if (@as(u32, r.x) + @as(u32, r.w) > @as(u32, caps.width_px)) return core.err_invalid_arg;
    if (@as(u32, r.y) + @as(u32, r.h) > @as(u32, caps.height_px)) return core.err_invalid_arg;
    return core.err_ok;
}

/// The pixel `display_clear` writes: the low 16 bits of the caller's colour.
pub fn rgb565Of(color: u32) u16 {
    return @truncate(color & lcd.rgb565_mask);
}

/// How many pixels a clear touches.
pub fn pixelCount(fb: core.Fb) u32 {
    return @as(u32, fb.width_px) * @as(u32, fb.height_px);
}

/// Byte offset of a flushed rectangle's first row from the framebuffer base.
pub fn rowSpanOffset(fb: core.Fb, r: core.Rect) u32 {
    return @as(u32, r.y) * fb.stride_bytes;
}

/// Bytes a flushed rectangle's row span covers. Whole rows, never a strided
/// `w * h` slice: a superset of the rectangle, exact for the full-width
/// flushes the reader issues, and safe for a partial-width one.
pub fn rowSpanBytes(fb: core.Fb, r: core.Rect) u32 {
    return @as(u32, r.h) * fb.stride_bytes;
}

/// Bytes the whole framebuffer covers, which is what a clear has just dirtied.
pub fn wholeFbBytes(fb: core.Fb) u32 {
    return fb.stride_bytes * @as(u32, fb.height_px);
}

/// The framebuffer shape `internal_lcd_deinit` leaves behind: the pointer is
/// dropped so a stale descriptor cannot be painted through, and the geometry
/// is deliberately left as it was.
pub fn releasedFb(fb: core.Fb) core.Fb {
    var next = fb;
    next.pixels = null;
    return next;
}

comptime {
    // `Fb` leads with a pointer, so the caps snapshot is padded out to pointer
    // alignment ahead of it.
    std.debug.assert(@offsetOf(Ctx, "caps") == 0);
    std.debug.assert(@offsetOf(Ctx, "fb") == std.mem.alignForward(usize, @sizeOf(core.Caps), @alignOf(core.Fb)));
    std.debug.assert(@offsetOf(Ctx, "started") == @offsetOf(Ctx, "fb") + @sizeOf(core.Fb));
}
