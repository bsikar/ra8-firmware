//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the e-paper geometry helpers (internal/epaper_geom.zig,
//! RA8FW-569). Built as its own object in libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const geom = @import("internal/epaper_geom.zig");

const tag = "EPAPER";

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// `uint8_t ra8_epaper_bits_per_pixel(ra8_epaper_pixel_format_t pf)`.
export fn ra8_epaper_bits_per_pixel(pf: u8) u8 {
    return geom.bitsPerPixel(pf);
}

/// `ra8_err_t ra8_epaper_image_bytes(const ra8_epaper_area_t*, pf, size_t*)`.
export fn ra8_epaper_image_bytes(area: ?*const geom.Area, pf: u8, out_bytes: ?*usize) u16 {
    const a = area orelse return nullPtr("image_bytes: area null");
    const out = out_bytes orelse return nullPtr("image_bytes: out null");
    out.* = geom.imageBytes(a.*, pf) catch return common.k_ra8_err_invalid_size;
    return common.k_ra8_ok;
}

/// `bool ra8_epaper_area_is_aligned(const ra8_epaper_area_t*, pf)`.
export fn ra8_epaper_area_is_aligned(area: ?*const geom.Area, pf: u8) bool {
    const a = area orelse return false;
    return geom.isAligned(a.*, pf);
}

/// `ra8_err_t ra8_epaper_align_area(ra8_epaper_area_t*, pf, uint16_t panel_width)`.
export fn ra8_epaper_align_area(area: ?*geom.Area, pf: u8, panel_width: u16) u16 {
    const a = area orelse return nullPtr("align_area: area null");
    geom.alignArea(a, pf, panel_width) catch return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_epaper_waveform_cfg_for_lut(const char*, ra8_epaper_waveform_cfg_t*)`.
export fn ra8_epaper_waveform_cfg_for_lut(lut_version: ?[*:0]const u8, out_cfg: ?*geom.Waveform) u16 {
    const lut = lut_version orelse return nullPtr("waveform_cfg: lut null");
    const out = out_cfg orelse return nullPtr("waveform_cfg: out null");
    out.* = geom.waveformForLut(lut);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_epaper_validate_cfg(const ra8_epaper_cfg_t* cfg)`.
export fn ra8_epaper_validate_cfg(cfg: ?*const geom.Cfg) u16 {
    const c = cfg orelse return nullPtr("validate_cfg: cfg null");
    geom.validateCfg(c) catch return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

/// `bool ra8_epaper_geometry_agrees(const ra8_epaper_dev_info_t*, const ra8_epaper_cfg_t*)`.
export fn ra8_epaper_geometry_agrees(info: ?*const geom.InfoHead, cfg: ?*const geom.Cfg) bool {
    const i = info orelse return false;
    const c = cfg orelse return false;
    return i.panel_width == c.panel_width and i.panel_height == c.panel_height;
}
