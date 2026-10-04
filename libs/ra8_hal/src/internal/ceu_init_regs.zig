//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CEU geometry, format and destination register packing (RA8FW-593).
//! Pure: registers are written through an ops value with write32(off, v).
//! HUM Ch 60.2.1 to 60.2.22, p 3638 to 3669.

pub const off_capcr: usize = 0x04;
pub const off_camcr: usize = 0x08;
pub const off_cmcyr: usize = 0x0C;
pub const off_camor: usize = 0x10;
pub const off_capwr: usize = 0x14;
pub const off_caifr: usize = 0x18;
pub const off_cflcr: usize = 0x30;
pub const off_cfszr: usize = 0x34;
pub const off_cdwdr: usize = 0x38;
pub const off_cfwcr: usize = 0x5C;
pub const off_clfcr: usize = 0x60;
pub const off_cdocr: usize = 0x64;
pub const off_cetcr: usize = 0x74;

/// `k_ra8_ceu_fmt_data_enable` (variable-length JPEG framing).
pub const fmt_data_enable: u8 = 2;
/// `k_ra8_ceu_capture_continuous`.
pub const capture_continuous: u8 = 1;

pub const ByteSwap = extern struct { swap_8_bit: bool, swap_16_bit: bool, swap_32_bit: bool };
pub const Edge = extern struct { data: u8, hsync: u8, vsync: u8, field: u8 };
pub const Scale = extern struct {
    h_mantissa: u16,
    h_fraction: u16,
    v_mantissa: u16,
    v_fraction: u16,
    h_output_clip: u16,
    v_output_clip: u16,
};

/// `ra8_ceu_config_t`; the enum fields are `enum : uint8_t`.
pub const Config = extern struct {
    width_px: u16,
    height_px: u16,
    x_start_px: u16,
    y_start_px: u16,
    x_capture_px: u16,
    y_capture_lines: u16,
    dst_stride: u16,
    frame_drop: u8,
    bytes_per_pixel: u8,
    interrupts: u32,
    capture_format: u8,
    capture_mode: u8,
    data_bus: u8,
    hsync_polarity: u8,
    vsync_polarity: u8,
    field_polarity: u8,
    input_order: u8,
    output_format: u8,
    burst_mode: u8,
    first_field: u8,
    edge: Edge,
    byte_swap: ByteSwap,
    scale: Scale,
    interlace: bool,
    one_field_only: bool,
    bundle_write: bool,
    low_pass_filter: bool,
    image_area_size: u32,
};

fn sh(v: anytype, comptime n: u5) u32 {
    return @as(u32, v) << n;
}

fn bit(b: bool, comptime n: u5) u32 {
    return @as(u32, @intFromBool(b)) << n;
}

/// CAMCR, HUM 60.2.2 p 3638.
pub fn packCamcr(c: *const Config) u32 {
    return sh(c.hsync_polarity, 0) | sh(c.vsync_polarity, 1) | sh(c.capture_format, 4) |
        sh(c.input_order, 8) | sh(c.data_bus, 12) | sh(c.field_polarity, 16) |
        sh(c.edge.data, 24) | sh(c.edge.field, 25) | sh(c.edge.hsync, 26) | sh(c.edge.vsync, 27);
}

/// CAPCR, HUM 60.2.1.
pub fn packCapcr(c: *const Config) u32 {
    return bit(c.capture_mode == capture_continuous, 16) | sh(c.burst_mode, 20) | sh(c.frame_drop, 24);
}

/// CAIFR, HUM 60.2.7.
pub fn packCaifr(c: *const Config) u32 {
    return sh(c.first_field, 0) | bit(c.one_field_only, 4) | bit(c.interlace, 8);
}

/// CFLCR, HUM 60.2.10: HFRAC[11:0], HMANT[15:12], VFRAC[27:16], VMANT[31:28].
pub fn packCflcr(c: *const Config) u32 {
    const s = c.scale;
    return (sh(s.h_fraction, 0) & 0x0000_0FFF) | (sh(s.h_mantissa, 12) & 0x0000_F000) |
        (sh(s.v_fraction, 16) & 0x0FFF_0000) | (sh(s.v_mantissa, 28) & 0xF000_0000);
}

/// CFSZR, HUM 60.2.11 p 3653.
pub fn packCfszr(c: *const Config) u32 {
    return (sh(c.scale.h_output_clip, 0) & 0x0000_0FFF) | (sh(c.scale.v_output_clip, 16) & 0x0FFF_0000);
}

/// CDOCR, HUM 60.2.20 p 3662.
pub fn packCdocr(c: *const Config) u32 {
    const b = c.byte_swap;
    return bit(b.swap_8_bit, 0) | bit(b.swap_16_bit, 1) | bit(b.swap_32_bit, 2) |
        (sh(c.output_format, 4) & (1 << 4)) | bit(c.bundle_write, 16);
}

fn outputWidthPx(c: *const Config) u16 {
    if (c.scale.h_output_clip != 0) return c.scale.h_output_clip;
    if (c.x_capture_px != 0) return c.x_capture_px;
    return c.width_px;
}

/// Bytes per output line; 0 in data-enable (JPEG) mode.
pub fn minStrideBytes(c: *const Config) u32 {
    if (c.capture_format == fmt_data_enable) return 0;
    return @as(u32, outputWidthPx(c)) * c.bytes_per_pixel;
}

/// CMCYR, CAMOR, CAPWR (HUM 60.2.4 to 60.2.6). Data-enable fetch uses none
/// of them, so they are zeroed there.
pub fn programGeometry(regs: anytype, c: *const Config) void {
    if (c.capture_format == fmt_data_enable) {
        regs.write32(off_cmcyr, 0);
        regs.write32(off_camor, 0);
        regs.write32(off_capwr, 0);
        return;
    }
    regs.write32(off_cmcyr, sh(c.height_px, 16) | c.width_px);
    regs.write32(off_camor, sh(c.y_start_px, 16) | c.x_start_px);
    const hwdth = if (c.x_capture_px != 0) c.x_capture_px else c.width_px;
    const vwdth = if (c.y_capture_lines != 0) c.y_capture_lines else c.height_px;
    regs.write32(off_capwr, sh(vwdth, 16) | hwdth);
}

pub fn programFormat(regs: anytype, c: *const Config) void {
    regs.write32(off_cflcr, packCflcr(c));
    regs.write32(off_caifr, packCaifr(c));
    regs.write32(off_capcr, packCapcr(c));
    regs.write32(off_camcr, packCamcr(c));
}

/// CFSZR, CDWDR (falls back to the minimum stride), CFWCR, CLFCR, CDOCR, CETCR.
pub fn programDestination(regs: anytype, c: *const Config) void {
    regs.write32(off_cfszr, packCfszr(c));
    regs.write32(off_cdwdr, if (c.dst_stride != 0) c.dst_stride else minStrideBytes(c));
    regs.write32(off_cfwcr, 0);
    regs.write32(off_clfcr, @intFromBool(c.low_pass_filter));
    regs.write32(off_cdocr, packCdocr(c));
    regs.write32(off_cetcr, 0);
}
