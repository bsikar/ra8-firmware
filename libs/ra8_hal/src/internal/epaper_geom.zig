//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! E-paper geometry and config checks (no registers). Port of
//! ra8_epaper_geom.c (RA8FW-569); the C ABI lives in epaper_geom_abi.zig.

/// `ra8_epaper_pixel_format_t`.
pub const pf_1bpp: u8 = 0;
pub const pf_2bpp: u8 = 1;
pub const pf_4bpp: u8 = 2;
pub const pf_8bpp: u8 = 3;

/// `k_ra8_epaper_align_1bpp_px`: the 32-pixel X/W grid for 1 bpp.
pub const align_1bpp_px: u32 = 32;
/// `k_ra8_epaper_wf_mode_max`.
pub const wf_mode_max: u8 = 7;
pub const panel_max_dim: u16 = 4096;

/// LUT mode numbers picked by `waveformForLut`.
pub const lut_init: u8 = 0;
pub const lut_du: u8 = 1;
pub const lut_gc16: u8 = 2;
pub const lut_a2_m641: u8 = 4;
pub const lut_a2_generic: u8 = 6;

/// `ra8_epaper_area_t`.
pub const Area = extern struct { x: u16, y: u16, width: u16, height: u16 };

/// `ra8_epaper_waveform_cfg_t`.
pub const Waveform = extern struct { init: u8, du: u8, gc16: u8, a2: u8 };

/// `ra8_spi_bus_ops_t`: xfer8 then ctx.
pub const BusOps = extern struct { xfer8: ?*const anyopaque, ctx: ?*anyopaque };

/// `ra8_epaper_cfg_t`.
pub const Cfg = extern struct {
    bus: BusOps,
    waveform: Waveform,
    reset_pin: u16,
    busy_pin: u16,
    panel_width: u16,
    panel_height: u16,
};

/// The leading fields of `ra8_epaper_dev_info_t`.
pub const InfoHead = extern struct { panel_width: u16, panel_height: u16 };

comptime {
    const p = @sizeOf(usize);
    if (@offsetOf(Cfg, "waveform") != 2 * p) @compileError("ra8_epaper_cfg_t.waveform moved");
    if (@offsetOf(Cfg, "panel_width") != 2 * p + 8) @compileError("ra8_epaper_cfg_t.panel_width moved");
    if (@sizeOf(Area) != 8) @compileError("ra8_epaper_area_t size");
}

pub const AreaError = error{ ZeroSize, OutOfPanel, OffGrid };
pub const CfgError = error{Invalid};

pub fn bitsPerPixel(pf: u8) u8 {
    return switch (pf) {
        pf_1bpp => 1,
        pf_2bpp => 2,
        pf_4bpp => 4,
        else => 8,
    };
}

/// Bytes for `area` at `pf`; each row starts on a byte boundary.
pub fn imageBytes(area: Area, pf: u8) AreaError!usize {
    if (area.width == 0 or area.height == 0) return error.ZeroSize;
    const row = (@as(usize, area.width) * bitsPerPixel(pf) + 7) / 8;
    return row * area.height;
}

pub fn isAligned(area: Area, pf: u8) bool {
    if (pf != pf_1bpp) return true;
    return area.x % align_1bpp_px == 0 and area.width % align_1bpp_px == 0;
}

/// Grow a 1 bpp window outward onto the grid, clamped to the panel.
pub fn alignArea(area: *Area, pf: u8, panel_width: u16) AreaError!void {
    if (panel_width == 0) return error.OutOfPanel;
    const right = @as(u32, area.x) + area.width;
    if (right > panel_width) return error.OutOfPanel;
    if (pf != pf_1bpp) return;
    const grid = align_1bpp_px;
    const x_lo = (@as(u32, area.x) / grid) * grid;
    const x_hi = @min(((right + grid - 1) / grid) * grid, @as(u32, panel_width));
    if (x_hi <= x_lo or (x_hi - x_lo) % grid != 0) return error.OffGrid;
    area.x = @intCast(x_lo);
    area.width = @intCast(x_hi - x_lo);
}

/// The waveform for a LUT version string: A2 is mode 4 on "M641".
pub fn waveformForLut(lut_version: [*:0]const u8) Waveform {
    const m641 = "M641";
    var is_m641 = true;
    for (m641, 0..) |c, i| {
        if (lut_version[i] != c) {
            is_m641 = false;
            break;
        }
    }
    return .{
        .init = lut_init,
        .du = lut_du,
        .gc16 = lut_gc16,
        .a2 = if (is_m641) lut_a2_m641 else lut_a2_generic,
    };
}

pub fn validateWaveform(wf: Waveform) CfgError!void {
    if (wf.du == 0 or wf.gc16 == 0 or wf.a2 == 0) return error.Invalid;
    const top = @max(@max(wf.init, wf.du), @max(wf.gc16, wf.a2));
    if (top > wf_mode_max) return error.Invalid;
}

pub fn validateCfg(cfg: *const Cfg) CfgError!void {
    if (cfg.bus.xfer8 == null or cfg.panel_width == 0 or cfg.panel_height == 0) return error.Invalid;
    if (cfg.panel_width > panel_max_dim or cfg.panel_height > panel_max_dim) return error.Invalid;
    return validateWaveform(cfg.waveform);
}
