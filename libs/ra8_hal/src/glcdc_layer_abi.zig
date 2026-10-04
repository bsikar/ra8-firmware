//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the GLCDC layer, blend and CLUT functions in ra8_glcdc.h
//! (RA8FW-604). The register sequences are in internal/glcdc_layer.zig.

const common = @import("abi_common.zig");
const layer = @import("internal/glcdc_layer.zig");

const tag = "GLCDC";

const Mmio = struct {
    fn reg(off: usize) *volatile u32 {
        return @ptrFromInt(layer.base + off);
    }
    pub fn read32(_: Mmio, off: usize) u32 {
        return reg(off).*;
    }
    pub fn write32(_: Mmio, off: usize, value: u32) void {
        reg(off).* = value;
    }
};

const C = struct {
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn infoVal(_: C, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
};

export fn ra8_glcdc_set_layer2(cfg: ?*const layer.Layer2Cfg) u16 {
    return layer.setLayer2(Mmio{}, C{}, cfg);
}

export fn ra8_glcdc_set_blend(mode: u8, global_alpha: u8) u16 {
    return layer.setBlend(Mmio{}, mode, global_alpha);
}

export fn ra8_glcdc_set_background_color(argb: u32) u16 {
    return layer.setBackgroundColor(Mmio{}, argb);
}

export fn ra8_glcdc_layer1_show(fb_addr: usize) u16 {
    return layer.layer1Show(Mmio{}, @truncate(fb_addr));
}

export fn ra8_glcdc_layer2_chroma_key_enable(key_rgb888: u32) u16 {
    return layer.layer2ChromaKeyEnable(Mmio{}, key_rgb888);
}

export fn ra8_glcdc_layer2_show(fb_addr: usize, panel_x: u16, panel_y: u16, fb_w: u16, fb_h: u16) u16 {
    return layer.layer2Show(Mmio{}, @truncate(fb_addr), panel_x, panel_y, fb_w, fb_h);
}

export fn ra8_glcdc_set_clut_double_buffered(layer_idx: u8, clut: ?[*]const u32, entries: u32, swap_now: bool) u16 {
    return layer.setClutDoubleBuffered(Mmio{}, C{}, layer_idx, clut, entries, swap_now);
}
