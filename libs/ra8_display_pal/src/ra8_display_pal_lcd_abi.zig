//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the LCD backend of the display PAL, `inc/ra8_display_pal_lcd.h`:
//! the `display_backend_iface` rows the GLCDC panel is driven through, the
//! vtable a caller binds by address, and the typed `display_pal_bind_glcdc`
//! helper. The decisions live in `internal/lcd.zig`; this file owns the
//! exported symbols, the module-static context, the seven-step bring-up and
//! the calls out to the board, the GLCDC HAL and the cache HAL.
//!
//! Two compile-time behaviours the C carried as per-app preprocessor flags are
//! build options here, because one archive per CPU is linked into every app:
//! `off-target` drops the ARM barrier for the host, and
//! `boot-enable-cache-mpu` keeps the D-cache clean that a cacheable
//! framebuffer needs. Both default off the target, and the cache default is
//! the safe direction: cleaning with the cache disabled costs cycles, while
//! skipping it with the cache enabled shows stale pixels.

const std = @import("std");
const build_config = @import("build_config");
const impl = @import("internal/lcd.zig");
const core = impl.core;
const pal = @import("ra8_display_pal_abi.zig");

/// Log tag on this backend's lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_display_pal_lcd";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

extern fn ra8_board_lcd_panel_power_on() u16;
extern fn ra8_board_glcdc_init(fmt: u8) u16;
extern fn ra8_delay_ms(ms: u32) void;
extern fn ra8_glcdc_init(cfg: *const GlcdcConfig) u16;
extern fn ra8_glcdc_set_background_color(argb: u32) u16;
extern fn ra8_glcdc_start(enable: bool) u16;
extern fn ra8_glcdc_layer1_show(fb_addr: usize) u16;
extern fn ra8_glcdc_deinit() u16;
extern fn ra8_cache_dcache_clean_by_addr(addr: ?*const anyopaque, size: u32) u16;

/// `k_ra8_board_glcdc_fmt_rgb888`: the parallel-RGB bus format the panel is
/// wired for, which is independent of the RGB565 framebuffer format.
const board_glcdc_fmt_rgb888: u8 = 0;
/// `k_ra8_glcdc_fmt_rgb565`, the GR1 plane's `FLM6.FORMAT` code.
const glcdc_fmt_rgb565: u8 = 0x2;

/// `ra8_glcdc_timing_t`: the panel's raw RGB timing, supplied by the board BSP
/// and passed through by value.
pub const GlcdcTiming = extern struct {
    h_active: u16 = 0,
    h_front: u16 = 0,
    h_back: u16 = 0,
    h_sync: u16 = 0,
    v_active: u16 = 0,
    v_front: u16 = 0,
    v_back: u16 = 0,
    v_sync: u16 = 0,
};

/// `ra8_glcdc_config_t`: what the GLCDC HAL is programmed from.
pub const GlcdcConfig = extern struct {
    framebuffer_addr: u32 = 0,
    width_px: u16 = 0,
    height_px: u16 = 0,
    format: u8 = 0,
    timing: GlcdcTiming = .{},
};

/// `display_fb_cfg_t`: the half of a `display_cfg_t` an application owns.
pub const FbCfg = extern struct {
    pixels: ?*anyopaque = null,
    bytes: u32 = 0,
    width_px: u16 = 0,
    height_px: u16 = 0,
    pixfmt: u8 = 0,
};

comptime {
    std.debug.assert(@sizeOf(GlcdcTiming) == 16);
    std.debug.assert(@offsetOf(GlcdcConfig, "width_px") == 4);
    std.debug.assert(@offsetOf(GlcdcConfig, "format") == 8);
    std.debug.assert(@offsetOf(GlcdcConfig, "timing") == 10);
    std.debug.assert(@offsetOf(FbCfg, "bytes") == @sizeOf(usize));
    std.debug.assert(@offsetOf(FbCfg, "width_px") == @sizeOf(usize) + 4);
    std.debug.assert(@offsetOf(FbCfg, "pixfmt") == @sizeOf(usize) + 8);
}

/// The one LCD context: one display per board, exactly as the C's `s_lcd_ctx`.
var s_lcd_ctx: impl.Ctx = .{};

/// Log a rejected pointer the way `RA8_CHECK_NULL_PTR` did, then answer
/// `k_ra8_err_null_ptr`.
fn nullPtr(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return core.err_null_ptr;
}

/// `internal_lcd_dsb`: make prior pixel writes observable to the GLCDC's AXI
/// master before the next scan. The host build has no such master and no
/// ARMv8-M instruction to emit.
/// `dsb sy` is the assembler spelling of the C's `dsb 0xF`: option 0xF is SY.
inline fn dsb() void {
    if (!build_config.off_target) {
        asm volatile ("dsb sy" ::: .{ .memory = true });
    }
}

/// The D-cache clean a cacheable framebuffer needs before the GLCDC scans it
/// over AXI. The CPU paints through the write-back L1, which the GLCDC never
/// sees, so the painted rows are written back here. Compiled out when the
/// archive is built for a target with no cache enabled.
fn cleanDCache(addr: ?*const anyopaque, bytes: u32) void {
    if (build_config.boot_enable_cache_mpu) {
        _ = ra8_cache_dcache_clean_by_addr(addr, bytes);
    }
}

/// `internal_lcd_bringup_panel`: the seven steps, each bailing out on the
/// first failure. A non-ok return means the panel may be in any state and the
/// caller must not call deinit.
fn bringupPanel(cfg: *const pal.Config, timing: *const GlcdcTiming) u16 {
    var err = ra8_board_lcd_panel_power_on();
    if (err != core.err_ok) return err;

    err = ra8_board_glcdc_init(board_glcdc_fmt_rgb888);
    if (err != core.err_ok) return err;

    ra8_delay_ms(impl.lcd.settle_ms);

    const glcdc_cfg: GlcdcConfig = .{
        .framebuffer_addr = @truncate(@intFromPtr(cfg.framebuffer)),
        .width_px = cfg.width_px,
        .height_px = cfg.height_px,
        .format = glcdc_fmt_rgb565,
        .timing = timing.*,
    };
    err = ra8_glcdc_init(&glcdc_cfg);
    if (err != core.err_ok) return err;

    err = ra8_glcdc_set_background_color(impl.lcd.bg_color_black);
    if (err != core.err_ok) return err;

    err = ra8_glcdc_start(true);
    if (err != core.err_ok) return err;

    return ra8_glcdc_layer1_show(@intFromPtr(cfg.framebuffer));
}

fn lcdInit(cfg: ?*const pal.Config, out_ctx: ?*?*anyopaque) callconv(.c) u16 {
    const config = cfg orelse return nullPtr("cfg");
    const out = out_ctx orelse return nullPtr("out_ctx");

    const view: core.CfgView = .{
        .has_framebuffer = config.framebuffer != null,
        .width_px = config.width_px,
        .height_px = config.height_px,
        .pixfmt = config.pixfmt,
        .has_panel_timing = config.panel_timing != null,
        .framebuffer_bytes = config.framebuffer_bytes,
    };
    const v = impl.validateCfg(view);
    if (v != core.err_ok) {
        if (v == core.err_null_ptr) ra8_log_emit_error(tag, "cfg->framebuffer");
        return v;
    }
    if (s_lcd_ctx.started) {
        ra8_log_emit_error(tag, "internal_lcd_init: already started");
        return core.err_busy;
    }

    const timing: *const GlcdcTiming = @ptrCast(@alignCast(config.panel_timing.?));
    const err = bringupPanel(config, timing);
    if (err != core.err_ok) return err;

    s_lcd_ctx.caps = impl.capsFor(config.width_px, config.height_px);
    s_lcd_ctx.fb = impl.fbFor(config.framebuffer, config.width_px, config.height_px);
    s_lcd_ctx.started = true;
    out.* = &s_lcd_ctx;
    return core.err_ok;
}

fn lcdGetCaps(ctx: ?*const anyopaque, out: ?*core.Caps) callconv(.c) u16 {
    const context: *const impl.Ctx = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const destination = out orelse return nullPtr("out");
    destination.* = context.caps;
    return core.err_ok;
}

fn lcdGetFramebuffer(ctx: ?*anyopaque, out: ?*core.Fb) callconv(.c) u16 {
    const context: *const impl.Ctx = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const destination = out orelse return nullPtr("out");
    destination.* = context.fb;
    return core.err_ok;
}

/// An LCD scans continuously, so a flush commits nothing: the caller's writes
/// land on the next scan. The rectangle is still validated, so code written
/// against the backend-agnostic API learns about an out-of-bounds rectangle
/// here rather than first discovering it on e-ink. The hint is informational.
fn lcdFlush(ctx: ?*anyopaque, rect: core.Rect, hint: u8) callconv(.c) u16 {
    _ = hint;
    const context: *const impl.Ctx = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const v = impl.checkRect(context.caps, rect);
    if (v != core.err_ok) return v;

    const base: ?[*]const u8 = @ptrCast(context.fb.pixels);
    if (base) |pixels| {
        cleanDCache(pixels + impl.rowSpanOffset(context.fb, rect), impl.rowSpanBytes(context.fb, rect));
    }
    dsb();
    return core.err_ok;
}

/// A CPU loop rather than DMA: the simplest correct clear.
fn lcdClear(ctx: ?*anyopaque, color: u32) callconv(.c) u16 {
    const context: *impl.Ctx = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const pixels: ?[*]u16 = @ptrCast(@alignCast(context.fb.pixels));
    if (pixels) |buffer| {
        const rgb565 = impl.rgb565Of(color);
        for (buffer[0..impl.pixelCount(context.fb)]) |*pixel| pixel.* = rgb565;
        cleanDCache(buffer, impl.wholeFbBytes(context.fb));
    }
    dsb();
    return core.err_ok;
}

/// Tear the GLCDC down and drop the framebuffer. The GPIO pins claimed during
/// `ra8_board_lcd_panel_power_on` are deliberately left claimed: a follow-up
/// init either resets the chip first or stays bound. The HAL's error is
/// returned, but the context is dropped either way.
fn lcdDeinit(ctx: ?*anyopaque) callconv(.c) u16 {
    const context: *impl.Ctx = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const err = ra8_glcdc_deinit();
    context.started = false;
    context.fb = impl.releasedFb(context.fb);
    return err;
}

/// The vtable a caller binds by address through `display_cfg_t.iface`.
pub export const k_display_backend_lcd_ra8_glcdc: pal.BackendIface = .{
    .init = &lcdInit,
    .get_caps = &lcdGetCaps,
    .get_framebuffer = &lcdGetFramebuffer,
    .flush = &lcdFlush,
    .clear = &lcdClear,
    .deinit = &lcdDeinit,
};

/// The typed counterpart to filling a `display_cfg_t` by hand: the caller
/// supplies only what it owns, so a GLCDC bind cannot be handed an e-ink panel
/// descriptor through an untyped pointer.
pub export fn display_pal_bind_glcdc(
    out: ?*?*pal.Handle,
    fb: ?*const FbCfg,
    timing: ?*const GlcdcTiming,
) callconv(.c) u16 {
    const destination = out orelse return nullPtr("out must not be nullptr");
    const framebuffer = fb orelse return nullPtr("fb must not be nullptr");
    if (framebuffer.pixels == null) return nullPtr("fb->pixels must not be nullptr");
    const panel_timing = timing orelse return nullPtr("timing must not be nullptr");

    const cfg: pal.Config = .{
        .iface = &k_display_backend_lcd_ra8_glcdc,
        .framebuffer = framebuffer.pixels,
        .framebuffer_bytes = framebuffer.bytes,
        .width_px = framebuffer.width_px,
        .height_px = framebuffer.height_px,
        .pixfmt = framebuffer.pixfmt,
        .panel_timing = panel_timing,
    };
    return pal.display_init(&cfg, destination);
}

/// Test-only reset of the module-static context.
pub fn testResetContext() void {
    s_lcd_ctx = .{};
}
