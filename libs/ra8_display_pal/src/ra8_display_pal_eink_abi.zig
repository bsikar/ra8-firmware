//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C boundary for the e-ink (IT8951) panel backend: the `ra8_epaper` calls,
//! the bounded row scratch, the six vtable rows and the exported backend
//! vtable `k_display_backend_eink_it8951`. Every decision it makes lives in
//! `internal/eink.zig`.

const std = @import("std");
const implementation = @import("internal/eink.zig");
const core = implementation.core;
const Waveform = implementation.Waveform;
const PixelFormat = implementation.PixelFormat;

const tag: [*:0]const u8 = "ra8_display_pal_eink";

// ---------------------------------------------------------------------------
// C the backend leans on
// ---------------------------------------------------------------------------

extern fn ra8_log_emit_error(tag: [*:0]const u8, msg: [*:0]const u8) void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, msg: [*:0]const u8) void;

/// `ra8_epaper_area_t`.
const EpaperArea = extern struct {
    x: u16,
    y: u16,
    width: u16,
    height: u16,
};

comptime {
    // The area descriptor and the two mode enums are the shape
    // `inc/ra8_display_pal_eink.h`'s panel traffic assumes.
    std.debug.assert(@sizeOf(EpaperArea) == 8);
    std.debug.assert(@offsetOf(EpaperArea, "width") == 4);
    std.debug.assert(@offsetOf(EpaperArea, "height") == 6);
    std.debug.assert(@backingInt(Waveform.gc16) == 2);
    std.debug.assert(@backingInt(PixelFormat.bpp4) == 2);
}

extern fn ra8_epaper_init(cfg: ?*const anyopaque) u16;
extern fn ra8_epaper_image_bytes(area: *const EpaperArea, pf: u32, out_bytes: *usize) u16;
extern fn ra8_epaper_load_image(
    area: *const EpaperArea,
    buf: [*]const u8,
    buf_len: usize,
    pf: u32,
    endian: u32,
) u16;
extern fn ra8_epaper_display_area(area: *const EpaperArea, waveform: u32) u16;
extern fn ra8_epaper_sleep() u16;

/// `k_ra8_epaper_endian_little`.
const endian_little: u32 = 0;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/// Single e-ink backend context.
var s_eink_ctx: implementation.Ctx = .{};

/// Bounded RGB565 to 4 bpp conversion scratch, one panel row.
///
/// Sized for a full-width row at 8 bpp even though the flush path packs at
/// 4 bpp, so the buffer stays correct if a future caller needs the wider
/// depth. At 4 bpp only the first `ceil(width / 2)` bytes are used. Written by
/// the flush path only, and not reentrant.
var s_eink_line: [implementation.eink.line_max_px]u8 = undefined;

// ---------------------------------------------------------------------------
// The view the vtable rows judge
// ---------------------------------------------------------------------------

/// `display_cfg_t`, as far as a panel backend reads it.
const Cfg = extern struct {
    iface: ?*const anyopaque,
    framebuffer: ?*anyopaque,
    framebuffer_bytes: u32,
    width_px: u16,
    height_px: u16,
    pixfmt: u8,
    panel_timing: ?*const anyopaque,
};

fn viewOf(cfg: *const Cfg) core.CfgView {
    return .{
        .has_framebuffer = cfg.framebuffer != null,
        .width_px = cfg.width_px,
        .height_px = cfg.height_px,
        .pixfmt = cfg.pixfmt,
        .has_panel_timing = cfg.panel_timing != null,
        .framebuffer_bytes = cfg.framebuffer_bytes,
    };
}

// ---------------------------------------------------------------------------
// Conversion + streaming
// ---------------------------------------------------------------------------

/// Convert and stream each row of `rect` into the IT8951 frame RAM.
///
/// The byte count comes from `ra8_epaper_image_bytes` rather than being
/// recomputed here, so the PAL's packing and the driver's expectation cannot
/// drift apart. The panel is refreshed once by the caller after all rows land.
fn loadRect(ctx: *const implementation.Ctx, rect: core.Rect) u16 {
    const pixels: [*]const u16 = @ptrCast(@alignCast(ctx.fb.pixels.?));
    var row: u16 = 0;
    while (row < rect.h) : (row += 1) {
        const first = implementation.rowOffsetPx(ctx.fb.width_px, rect, row);
        const span = pixels[first .. first + rect.w];
        const bytes = implementation.packedRowBytes(rect.w);
        implementation.packRow4bpp(span, s_eink_line[0..bytes]);

        const area: EpaperArea = .{ .x = rect.x, .y = rect.y + row, .width = rect.w, .height = 1 };
        var need: usize = 0;
        const serr = ra8_epaper_image_bytes(&area, @backingInt(PixelFormat.bpp4), &need);
        if (serr != core.err_ok) return serr;
        const err = ra8_epaper_load_image(
            &area,
            &s_eink_line,
            need,
            @backingInt(PixelFormat.bpp4),
            endian_little,
        );
        if (err != core.err_ok) return err;
    }
    return core.err_ok;
}

// ---------------------------------------------------------------------------
// Vtable rows
// ---------------------------------------------------------------------------

fn einkInit(cfg: ?*const Cfg, out_ctx: ?*?*anyopaque) callconv(.c) u16 {
    const c = cfg orelse return core.err_null_ptr;
    const out = out_ctx orelse return core.err_null_ptr;
    const v = implementation.validateCfg(viewOf(c));
    if (v != core.err_ok) return v;
    if (s_eink_ctx.initialised) {
        ra8_log_emit_error(tag, "eink init: already initialised");
        return core.err_busy;
    }
    const err = ra8_epaper_init(c.panel_timing);
    if (err != core.err_ok) {
        ra8_log_emit_error(tag, "eink init: ra8_epaper_init failed");
        return err;
    }
    s_eink_ctx = .{
        .caps = implementation.capsFor(c.width_px, c.height_px),
        .fb = implementation.fbFor(c.framebuffer, c.width_px, c.height_px),
        .initialised = true,
    };
    out.* = &s_eink_ctx;
    ra8_log_emit_info(tag, "eink init: IT8951 backend bound");
    return core.err_ok;
}

fn einkGetCaps(ctx: ?*const implementation.Ctx, out: ?*core.Caps) callconv(.c) u16 {
    const c = ctx orelse return core.err_null_ptr;
    const o = out orelse return core.err_null_ptr;
    o.* = c.caps;
    return core.err_ok;
}

fn einkGetFramebuffer(ctx: ?*implementation.Ctx, out: ?*core.Fb) callconv(.c) u16 {
    const c = ctx orelse return core.err_null_ptr;
    const o = out orelse return core.err_null_ptr;
    o.* = c.fb;
    return core.err_ok;
}

fn einkFlush(ctx: ?*implementation.Ctx, rect: core.Rect, hint: u8) callconv(.c) u16 {
    const c = ctx orelse return core.err_null_ptr;
    const v = implementation.checkRect(c.caps, rect);
    if (v != core.err_ok) return v;
    const lerr = loadRect(c, rect);
    if (lerr != core.err_ok) return lerr;
    const area: EpaperArea = .{ .x = rect.x, .y = rect.y, .width = rect.w, .height = rect.h };
    return ra8_epaper_display_area(&area, @backingInt(implementation.waveformFor(hint)));
}

/// Per the PAL contract `display_clear` only writes the framebuffer; the
/// caller follows with `display_flush`. So this mirrors the LCD backend.
fn einkClear(ctx: ?*implementation.Ctx, color: u32) callconv(.c) u16 {
    const c = ctx orelse return core.err_null_ptr;
    const pixels: [*]u16 = @ptrCast(@alignCast(c.fb.pixels.?));
    const total = implementation.pixelCount(c.fb);
    const rgb565: u16 = @truncate(color);
    @memset(pixels[0..total], rgb565);
    return core.err_ok;
}

/// Sleep the panel, which returns the driver to its uninitialised state so a
/// fresh init works, then drop the context.
fn einkDeinit(ctx: ?*implementation.Ctx) callconv(.c) u16 {
    const c = ctx orelse return core.err_null_ptr;
    const err = ra8_epaper_sleep();
    c.* = implementation.released(c.*);
    return err;
}

// ---------------------------------------------------------------------------
// Public vtable + helper
// ---------------------------------------------------------------------------

/// `display_backend_iface_t` for the IT8951 e-paper panel.
const Iface = extern struct {
    init: *const fn (?*const Cfg, ?*?*anyopaque) callconv(.c) u16,
    get_caps: *const fn (?*const implementation.Ctx, ?*core.Caps) callconv(.c) u16,
    get_framebuffer: *const fn (?*implementation.Ctx, ?*core.Fb) callconv(.c) u16,
    flush: *const fn (?*implementation.Ctx, core.Rect, u8) callconv(.c) u16,
    clear: *const fn (?*implementation.Ctx, u32) callconv(.c) u16,
    deinit: *const fn (?*implementation.Ctx) callconv(.c) u16,
};

pub export const k_display_backend_eink_it8951: Iface = .{
    .init = einkInit,
    .get_caps = einkGetCaps,
    .get_framebuffer = einkGetFramebuffer,
    .flush = einkFlush,
    .clear = einkClear,
    .deinit = einkDeinit,
};

/// Rec.601 luma of one RGB565 pixel. Public because the host suite pins the
/// conversion directly.
pub export fn ra8_display_pal_eink_luma_from_rgb565(px: u16) callconv(.c) u8 {
    return implementation.lumaFromRgb565(px);
}

/// Test seam: drop the module context so one test cannot leak into the next.
pub fn testResetContext() void {
    s_eink_ctx = .{};
}
