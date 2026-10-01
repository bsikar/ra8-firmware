//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A flat colour on the J1 panel with no framebuffer at all.
//!
//! The controller composes background x layer2 x layer1. Leave both graphics
//! layers invisible and the background plane fills the active area on its
//! own, so nothing has to allocate or scan out 1.2 MiB of SDRAM to light the
//! glass. Pin routing and panel power are not done here.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;

/// The configuration handed to the controller, re-exported so a host suite
/// can stand a fake behind the controller seam.
pub const Cfg = hal.GlcdcCfg;

/// ER-TFT070-6 active area.
pub const Geometry = struct {
    pub const width_px: u16 = 1024;
    pub const height_px: u16 = 600;
};

/// ER-TFT070-6 blanking, per the LVGL EK-RA8D2 reference for this panel.
/// These differ from the generic 1024x600 numbers: h_back 140 -> 160,
/// h_sync 20 -> 4, v_back 20 -> 23.
pub const Porch = struct {
    pub const h_front: u16 = 160;
    pub const h_back: u16 = 160;
    pub const h_sync: u16 = 4;
    pub const v_front: u16 = 12;
    pub const v_back: u16 = 23;
    pub const v_sync: u16 = 3;
};

/// What this path tells the controller about memory it never reads.
pub const Plane = struct {
    /// Address handed over when no framebuffer is in play.
    pub const no_framebuffer: u32 = 0;
    /// `k_ra8_glcdc_fmt_rgb565`. The format describes the graphics layers,
    /// which stay invisible here, so it changes nothing on the glass. It is
    /// stated because the controller validates the field.
    pub const format: u8 = 0x2;
};

/// This board's panel as one controller configuration.
pub fn config() Cfg {
    return .{
        .framebuffer_addr = Plane.no_framebuffer,
        .width_px = Geometry.width_px,
        .height_px = Geometry.height_px,
        .format = Plane.format,
        .timing = .{
            .h_active = Geometry.width_px,
            .h_front = Porch.h_front,
            .h_back = Porch.h_back,
            .h_sync = Porch.h_sync,
            .v_active = Geometry.height_px,
            .v_front = Porch.v_front,
            .v_back = Porch.v_back,
            .v_sync = Porch.v_sync,
        },
    };
}

/// Configure the controller for this panel, set the colour, start scanout.
pub fn begin(rgb888: u32) u32 {
    const cfg = config();
    var err = hal.ra8_glcdc_init(&cfg);
    if (err != Err.ok) return err;

    err = hal.ra8_glcdc_set_background_color(rgb888);
    if (err != Err.ok) return err;

    return hal.ra8_glcdc_start(true);
}

/// The background colour register is shadowed, so the write lands at the
/// next vertical sync and the panel never shows a half-updated colour.
pub fn set(rgb888: u32) u32 {
    return hal.ra8_glcdc_set_background_color(rgb888);
}
