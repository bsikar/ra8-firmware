//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GLCDC graphics layers, blend, background colour and double-buffered
//! CLUT (HUM Ch 63), RA8FW-604. Pure register sequences; the C ABI lives
//! in glcdc_layer_abi.zig. `regs` provides read32(off) and
//! write32(off, u32); `c` provides err(msg) and infoVal(msg, u32).

pub const base: usize = 0x4034_2000;
pub const off_gr1_clut0: usize = 0x0000;
pub const off_gr1_clut1: usize = 0x0400;
pub const off_gr2_clut0: usize = 0x0800;
pub const off_gr2_clut1: usize = 0x0C00;
pub const off_bg_en: usize = 0x1000;
pub const off_bg_bgc: usize = 0x1014;
pub const off_gr1_en: usize = 0x1100;
pub const off_gr1_flmrd: usize = 0x1104;
pub const off_gr1_saddr: usize = 0x110C;
pub const off_gr1_ab1: usize = 0x1120;
pub const off_gr1_ab7: usize = 0x1138;
pub const off_gr1_clutint: usize = 0x1150;
pub const off_gr2_en: usize = 0x1200;
pub const off_gr2_flmrd: usize = 0x1204;
pub const off_gr2_saddr: usize = 0x120C;
pub const off_gr2_flm3: usize = 0x1210;
pub const off_gr2_line: usize = 0x1218;
pub const off_gr2_fmt: usize = 0x121C;
pub const off_gr2_ab1: usize = 0x1220;
pub const off_gr2_ab2: usize = 0x1224;
pub const off_gr2_size: usize = 0x1228;
pub const off_gr2_ab4: usize = 0x122C;
pub const off_gr2_ab5: usize = 0x1230;
pub const off_gr2_ab7: usize = 0x1238;
pub const off_gr2_ab8: usize = 0x123C;
pub const off_gr2_ab9: usize = 0x1240;
pub const off_gr2_clutint: usize = 0x1250;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const null_ptr: u16 = 0x504;

pub const layer_count: u8 = 2;
pub const clut_entries: u32 = 256;
const dispsel_below: u32 = 1;
const dispsel_above: u32 = 2;
const dispsel_lower: u32 = 3;
const arcon: u32 = 0x1000;
const clutsel: u32 = 0x1_0000;
const ven_bg: u32 = 1 << 8;
const ven_gr: u32 = 1;
const ven_timeout: u32 = 0x4_0000;
const opaque_ab7: u32 = 0xFF << 16;
/// The C tries three CKON candidate bits at once (bits 0, 16, 24).
const ckon_guess: u32 = (1 << 0) | (1 << 16) | (1 << 24);
const fmt_rgb565: u32 = 2 << 28;
const axi_burst: u32 = 64;
const bpp_rgb565: u32 = 2;

/// `ra8_glcdc_layer2_cfg_t`, 20 bytes.
pub const Layer2Cfg = extern struct {
    framebuffer_addr: u32 = 0,
    line_stride_bytes: u32 = 0,
    width_px: u16 = 0,
    height_px: u16 = 0,
    pos_x: u16 = 0,
    pos_y: u16 = 0,
    format: u8 = 0,
    alpha: u8 = 0,
};

/// `ra8_glcdc_blend_mode_t`.
pub const blend_overwrite: u8 = 0;
pub const blend_normal: u8 = 1;
pub const blend_alpha: u8 = 2;

fn hiLo(hi: u32, lo: u32) u32 {
    return (hi << 16) | lo;
}

/// `ra8_glcdc_set_layer2`.
pub fn setLayer2(regs: anytype, c: anytype, cfg_opt: ?*const Layer2Cfg) u16 {
    const cfg = cfg_opt orelse {
        c.err("cfg must not be nullptr");
        return null_ptr;
    };
    regs.write32(off_gr2_fmt, cfg.format);
    regs.write32(off_gr2_saddr, cfg.framebuffer_addr);
    regs.write32(off_gr2_flm3, cfg.line_stride_bytes << 16);
    regs.write32(off_gr2_line, hiLo(cfg.height_px, cfg.width_px));
    regs.write32(off_gr2_size, hiLo(cfg.pos_x, cfg.width_px));
    regs.write32(off_gr2_ab2, hiLo(cfg.pos_y, cfg.height_px));
    regs.write32(off_gr2_ab5, hiLo(cfg.pos_x, cfg.width_px));
    regs.write32(off_gr2_ab4, hiLo(cfg.pos_y, cfg.height_px));
    regs.write32(off_gr2_ab7, @as(u32, cfg.alpha) << 16);
    regs.write32(off_gr2_ab1, dispsel_below);
    regs.write32(off_gr2_flmrd, 1);
    regs.write32(off_gr2_en, ven_gr);
    c.infoVal("set_layer2 fb", cfg.framebuffer_addr);
    return ok;
}

/// `ra8_glcdc_set_blend`.
pub fn setBlend(regs: anytype, mode: u8, global_alpha: u8) u16 {
    const ab1: u32 = switch (mode) {
        blend_overwrite => dispsel_below,
        blend_normal => dispsel_above,
        blend_alpha => dispsel_above | arcon,
        else => return invalid_arg,
    };
    regs.write32(off_gr1_ab1, ab1);
    regs.write32(off_gr1_ab7, @as(u32, global_alpha) << 16);
    return ok;
}

/// `ra8_glcdc_set_background_color`: latch VEN, wait (bounded) for the
/// hardware to clear it, then write BG_BGC.
pub fn setBackgroundColor(regs: anytype, argb: u32) u16 {
    regs.write32(off_bg_en, regs.read32(off_bg_en) | ven_bg);
    var i: u32 = 0;
    while (i < ven_timeout) : (i += 1) {
        if ((regs.read32(off_bg_en) & ven_bg) == 0) break;
    }
    regs.write32(off_bg_bgc, argb);
    return ok;
}

/// `ra8_glcdc_layer1_show`.
pub fn layer1Show(regs: anytype, fb_addr: u32) u16 {
    regs.write32(off_gr1_saddr, fb_addr);
    regs.write32(off_gr1_ab7, opaque_ab7);
    regs.write32(off_gr1_ab1, dispsel_lower);
    regs.write32(off_gr1_flmrd, 1);
    regs.write32(off_gr1_en, ven_gr);
    return ok;
}

/// `ra8_glcdc_layer2_chroma_key_enable`.
pub fn layer2ChromaKeyEnable(regs: anytype, key_rgb888: u32) u16 {
    regs.write32(off_gr2_ab8, 0xFF00_0000 | (key_rgb888 & 0x00FF_FFFF));
    regs.write32(off_gr2_ab9, 0);
    regs.write32(off_gr2_ab7, opaque_ab7 | ckon_guess);
    regs.write32(off_gr2_ab1, regs.read32(off_gr2_ab1) | arcon);
    regs.write32(off_gr2_en, ven_gr);
    return ok;
}

/// `ra8_glcdc_layer2_show`: RGB565 framebuffer at a panel position.
/// DATANUM and LNNUM wrap like the C's unsigned arithmetic.
pub fn layer2Show(regs: anytype, fb_addr: u32, panel_x: u16, panel_y: u16, fb_w: u16, fb_h: u16) u16 {
    const line_bytes = @as(u32, fb_w) * bpp_rgb565;
    regs.write32(off_gr2_fmt, fmt_rgb565);
    regs.write32(off_gr2_saddr, fb_addr);
    regs.write32(off_gr2_flm3, line_bytes << 16);
    regs.write32(off_gr2_line, ((@as(u32, fb_h) -% 1) << 16) | ((line_bytes / axi_burst) -% 1));
    regs.write32(off_gr2_size, hiLo(panel_x, fb_w));
    regs.write32(off_gr2_ab2, hiLo(panel_y, fb_h));
    regs.write32(off_gr2_ab4, hiLo(panel_y, fb_h));
    regs.write32(off_gr2_ab5, hiLo(panel_x, fb_w));
    regs.write32(off_gr2_ab7, opaque_ab7);
    regs.write32(off_gr2_ab1, dispsel_lower);
    regs.write32(off_gr2_flmrd, 1);
    regs.write32(off_gr2_en, ven_gr);
    return ok;
}

fn clutPlane(layer: u8, plane: u32) usize {
    if (layer == 0) return if (plane == 0) off_gr1_clut0 else off_gr1_clut1;
    return if (plane == 0) off_gr2_clut0 else off_gr2_clut1;
}

fn clutintOff(layer: u8) usize {
    return if (layer == 0) off_gr1_clutint else off_gr2_clutint;
}

/// `ra8_glcdc_set_clut_double_buffered`: fill the inactive plane, then
/// optionally flip CLUTINT.SEL to it.
pub fn setClutDoubleBuffered(regs: anytype, c: anytype, layer: u8, clut_opt: ?[*]const u32, entries: u32, swap_now: bool) u16 {
    const clut = clut_opt orelse {
        c.err("clut must not be nullptr");
        return null_ptr;
    };
    if (layer >= layer_count) return invalid_arg;
    if (entries == 0 or entries > clut_entries) return invalid_arg;
    const int_off = clutintOff(layer);
    const active = (regs.read32(int_off) & clutsel) >> 16;
    const target: u32 = if (active != 0) 0 else 1;
    const dst = clutPlane(layer, target);
    for (clut[0..entries], 0..) |value, i| regs.write32(dst + i * 4, value);
    if (swap_now) regs.write32(int_off, (regs.read32(int_off) & ~clutsel) | (target << 16));
    return ok;
}
