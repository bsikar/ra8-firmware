//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Transport-free core of the OmniVision OV5640 driver: the register map, the
//! board-qualified VGA DVP scene table, the JPEG overlay, the masked readback
//! expectations and the pure decode arithmetic behind
//! `ra8_ov5640_jpeg_status_get`.
//!
//! Nothing here performs I/O. Every SCCB access and every delay is a
//! caller-injected seam owned by `ra8_ov5640_abi.zig`, so these functions are
//! testable with no bus, no mock and no archive.

const std = @import("std");

/// OV5640 SCCB register addresses (OmniVision device data, not RA8D2 MMIO).
pub const reg = struct {
    pub const system_reset00: u16 = 0x3000;
    pub const system_reset02: u16 = 0x3002;
    pub const clock_enable02: u16 = 0x3006;
    pub const sw_reset: u16 = 0x3008;
    pub const chip_id_hi: u16 = 0x300A;
    pub const chip_id_lo: u16 = 0x300B;
    pub const pll_bit_mode: u16 = 0x3034;
    pub const pll_sys_div: u16 = 0x3035;
    pub const pll_multiplier: u16 = 0x3036;
    pub const pll_pre_div: u16 = 0x3037;
    pub const pll_bypass: u16 = 0x3039;
    pub const clock_select: u16 = 0x3103;
    pub const clock_root: u16 = 0x3108;
    pub const timing_y_start_hi: u16 = 0x3802;
    pub const timing_y_start_lo: u16 = 0x3803;
    pub const timing_y_end_hi: u16 = 0x3806;
    pub const timing_y_end_lo: u16 = 0x3807;
    pub const timing_hts_hi: u16 = 0x380C;
    pub const timing_hts_lo: u16 = 0x380D;
    pub const timing_vts_hi: u16 = 0x380E;
    pub const timing_vts_lo: u16 = 0x380F;
    pub const timing_y_offset_hi: u16 = 0x3812;
    pub const timing_y_offset_lo: u16 = 0x3813;
    pub const timing_tc_reg20: u16 = 0x3820;
    pub const timing_tc_reg21: u16 = 0x3821;
    pub const pclk_divider: u16 = 0x3824;
    pub const format: u16 = 0x4300;
    pub const jpeg_ctrl00: u16 = 0x4400;
    pub const jpeg_ctrl01: u16 = 0x4401;
    pub const jpeg_ctrl04: u16 = 0x4404;
    pub const jpeg_quality: u16 = 0x4407;
    pub const jpeg_length_hi: u16 = 0x4414;
    pub const jpeg_length_mid: u16 = 0x4415;
    pub const jpeg_length_lo: u16 = 0x4416;
    pub const jfifo_overflow: u16 = 0x4417;
    pub const jpeg_timing14: u16 = 0x4514;
    pub const jpeg_timing20: u16 = 0x4520;
    pub const vfifo_ctrl00: u16 = 0x4600;
    pub const compression_w_hi: u16 = 0x4602;
    pub const compression_w_lo: u16 = 0x4603;
    pub const compression_h_hi: u16 = 0x4604;
    pub const compression_h_lo: u16 = 0x4605;
    pub const vfifo_ctrl0b: u16 = 0x460B;
    pub const vfifo_ctrl0c: u16 = 0x460C;
    pub const jpeg_mode: u16 = 0x4713;
    pub const jpeg_ctrl1c: u16 = 0x471C;
    pub const href_minimum: u16 = 0x471F;
    pub const polarity_ctrl00: u16 = 0x4740;
    pub const isp_ctrl01: u16 = 0x5001;
    pub const isp_mux: u16 = 0x501F;
    pub const test_pattern: u16 = 0x503D;
};

/// OV5640 reset values, field masks and byte-wrangling constants.
pub const val = struct {
    pub const sw_reset_hold: u8 = 0x82;
    pub const sw_standby: u8 = 0x42;
    pub const sw_reset_wake: u8 = 0x02;
    pub const mcu_reset_hold: u8 = 0x20;
    pub const hi_byte_shift: u8 = 8;
    pub const byte_mask: u8 = 0xFF;
    pub const format_yuyv: u8 = 0x30;
    pub const isp_mux_yuv: u8 = 0x00;
    pub const jpeg_mode_dvp_2: u8 = 0x02;
    pub const jpeg_mode_mask: u8 = 0x07;
    pub const jpeg_sync_polarity_mask: u8 = 0x03;
    pub const jpeg_sync_polarity: u8 = 0x01;
    pub const jpeg_ctrl00: u8 = 0x81;
    pub const jpeg_ctrl01: u8 = 0x01;
    pub const jpeg_ctrl04: u8 = 0x24;
    pub const jpeg_ctrl1c: u8 = 0x50;
    pub const jpeg_timing14_vga: u8 = 0xAA;
    pub const jpeg_timing20_vga: u8 = 0x0B;
    pub const timing_tc_reg20_jpeg: u8 = 0x01;
    pub const jpeg_y_start_hi: u8 = 0x00;
    pub const jpeg_y_start_lo: u8 = 0x00;
    pub const jpeg_y_end_hi: u8 = 0x07;
    pub const jpeg_y_end_lo: u8 = 0x9F;
    pub const jpeg_hts_hi: u8 = 0x08;
    pub const jpeg_hts_lo: u8 = 0x0C;
    pub const jpeg_vts_hi: u8 = 0x03;
    pub const jpeg_vts_lo: u8 = 0xD8;
    pub const jpeg_y_offset_hi: u8 = 0x00;
    pub const jpeg_y_offset_lo: u8 = 0x08;
    pub const jpeg_clock_mask: u8 = 0x28;
    pub const vfifo_ctrl0b_jpeg: u8 = 0x35;
    pub const vfifo_ctrl0c_jpeg: u8 = 0x22;
    pub const pll_bit_mode_raw: u8 = 0x18;
    pub const pll_sys_div_raw: u8 = 0x21;
    pub const pll_multiplier_raw: u8 = 0x46;
    pub const pll_pre_div_raw: u8 = 0x13;
    pub const pll_bypass_disabled: u8 = 0x00;
    pub const clock_root_raw: u8 = 0x01;
    pub const clock_select_raw: u8 = 0x02;
    pub const pclk_divider_raw: u8 = 0x01;
    pub const jpeg_enable_mask: u8 = 0x20;
    pub const jpeg_reset_mask: u8 = 0x1C;
    pub const jpeg_quant_scale_mask: u8 = 0x3F;
    pub const jpeg_input_yuv422_mask: u8 = 0x80;
    pub const jpeg_header_mask: u8 = 0x20;
    pub const jfifo_overflow_mask: u8 = 0x01;
    pub const isp_scale_enable_mask: u8 = 0x20;
    pub const jpeg_vga_width_hi: u8 = 0x02;
    pub const jpeg_vga_width_lo: u8 = 0x80;
    pub const jpeg_vga_height_hi: u8 = 0x01;
    pub const jpeg_vga_height_lo: u8 = 0xE0;
};

/// Settle intervals the driver asks of the injected delay callback (ms).
pub const delay = struct {
    pub const reset_guard_ms: u32 = 100;
    pub const mcu_reset_ms: u32 = 10;
    pub const stream_settle_ms: u32 = 5;
    pub const cfg_settle_ms: u32 = 500;
};

/// The two legal seven-bit SCCB addresses, in probe order.
pub const addresses = [2]u8{ 0x3C, 0x3D };

/// Expected combined chip identifier (`k_ra8_ov5640_chip_id`).
pub const chip_id: u16 = 0x5640;

/// Highest legal raw JPEG quantization scale (`CTRL07` bits [5:0]).
pub const quant_scale_max: u8 = 0x3F;
/// Vendor reset-scale value programmed by the JPEG overlay.
pub const quant_scale_default: u8 = 0x0C;

/// Validated output modes (`ra8_ov5640_mode_t`).
pub const mode_vga_uyvy: u8 = 0;
/// Sensor-encoded VGA JPEG stream.
pub const mode_vga_jpeg: u8 = 1;

/// One SCCB register write: 16-bit address, 8-bit value.
pub const RegWrite = struct {
    reg: u16,
    val: u8,
};

/// One masked register readback expectation.
pub const RegExpect = struct {
    reg: u16,
    mask: u8,
    value: u8,
};

/// OV5640 DVP init: PLL from the 24 MHz XVCLK, VGA YUV422 live output.
/// Row order is part of the contract: the C suite asserts the wire sequence.
pub const vga_uyvy = [_]RegWrite{
    .{ .reg = 0x3103, .val = 0x11 },
    .{ .reg = 0x4740, .val = 0x20 },
    .{ .reg = 0x4050, .val = 0x6E },
    .{ .reg = 0x4051, .val = 0x8F },
    .{ .reg = 0x3103, .val = 0x02 },
    .{ .reg = 0x3017, .val = 0x7F },
    .{ .reg = 0x3018, .val = 0xFF },
    .{ .reg = 0x302C, .val = 0xC2 },
    .{ .reg = reg.pll_bit_mode, .val = val.pll_bit_mode_raw },
    .{ .reg = reg.pll_sys_div, .val = val.pll_sys_div_raw },
    .{ .reg = reg.pll_multiplier, .val = val.pll_multiplier_raw },
    .{ .reg = reg.pll_pre_div, .val = val.pll_pre_div_raw },
    .{ .reg = reg.clock_root, .val = val.clock_root_raw },
    .{ .reg = 0x3630, .val = 0x2E },
    .{ .reg = 0x3631, .val = 0x0E },
    .{ .reg = 0x3632, .val = 0xE2 },
    .{ .reg = 0x3633, .val = 0x23 },
    .{ .reg = 0x3621, .val = 0xE0 },
    .{ .reg = 0x3704, .val = 0xA0 },
    .{ .reg = 0x3703, .val = 0x5A },
    .{ .reg = 0x3715, .val = 0x78 },
    .{ .reg = 0x3717, .val = 0x01 },
    .{ .reg = 0x370B, .val = 0x60 },
    .{ .reg = 0x3705, .val = 0x1A },
    .{ .reg = 0x3905, .val = 0x02 },
    .{ .reg = 0x3906, .val = 0x10 },
    .{ .reg = 0x3901, .val = 0x0A },
    .{ .reg = 0x3731, .val = 0x12 },
    .{ .reg = 0x3600, .val = 0x08 },
    .{ .reg = 0x3601, .val = 0x33 },
    .{ .reg = 0x302D, .val = 0x60 },
    .{ .reg = 0x3620, .val = 0x52 },
    .{ .reg = 0x371B, .val = 0x20 },
    .{ .reg = 0x471C, .val = 0x50 },
    .{ .reg = 0x3A13, .val = 0x43 },
    .{ .reg = 0x3A18, .val = 0x00 },
    .{ .reg = 0x3A19, .val = 0xF8 },
    .{ .reg = 0x3635, .val = 0x1C },
    .{ .reg = 0x3636, .val = 0x03 },
    .{ .reg = 0x3634, .val = 0x40 },
    .{ .reg = 0x3622, .val = 0x01 },
    .{ .reg = 0x3C01, .val = 0xB4 },
    .{ .reg = 0x3C04, .val = 0x28 },
    .{ .reg = 0x3C05, .val = 0x98 },
    .{ .reg = 0x3C06, .val = 0x00 },
    .{ .reg = 0x3C07, .val = 0x08 },
    .{ .reg = 0x3C08, .val = 0x00 },
    .{ .reg = 0x3C09, .val = 0x1C },
    .{ .reg = 0x3C0A, .val = 0x9C },
    .{ .reg = 0x3C0B, .val = 0x40 },
    .{ .reg = 0x3618, .val = 0x00 },
    .{ .reg = 0x3612, .val = 0x29 },
    .{ .reg = 0x3708, .val = 0x64 },
    .{ .reg = 0x3709, .val = 0x52 },
    .{ .reg = 0x370C, .val = 0x03 },
    .{ .reg = 0x3A00, .val = 0x3C },
    .{ .reg = 0x3A02, .val = 0x05 },
    .{ .reg = 0x3A03, .val = 0xC4 },
    .{ .reg = 0x3A08, .val = 0x00 },
    .{ .reg = 0x3A09, .val = 0x93 },
    .{ .reg = 0x3A0A, .val = 0x00 },
    .{ .reg = 0x3A0B, .val = 0x7B },
    .{ .reg = 0x3A0D, .val = 0x08 },
    .{ .reg = 0x3A0E, .val = 0x06 },
    .{ .reg = 0x3A0F, .val = 0x30 },
    .{ .reg = 0x3A10, .val = 0x28 },
    .{ .reg = 0x3A11, .val = 0x60 },
    .{ .reg = 0x3A14, .val = 0x05 },
    .{ .reg = 0x3A15, .val = 0xC4 },
    .{ .reg = 0x3A1B, .val = 0x30 },
    .{ .reg = 0x3A1E, .val = 0x26 },
    .{ .reg = 0x3A1F, .val = 0x14 },
    .{ .reg = 0x3503, .val = 0x00 },
    .{ .reg = 0x3C00, .val = 0x04 },
    .{ .reg = 0x4001, .val = 0x02 },
    .{ .reg = 0x4004, .val = 0x02 },
    .{ .reg = 0x3808, .val = 0x02 },
    .{ .reg = 0x3809, .val = 0x80 },
    .{ .reg = 0x380A, .val = 0x01 },
    .{ .reg = 0x380B, .val = 0xE0 },
    .{ .reg = 0x380C, .val = 0x0C },
    .{ .reg = 0x380D, .val = 0x80 },
    .{ .reg = 0x380E, .val = 0x07 },
    .{ .reg = 0x380F, .val = 0xD0 },
    .{ .reg = 0x3800, .val = 0x00 },
    .{ .reg = 0x3801, .val = 0x00 },
    .{ .reg = 0x3802, .val = 0x00 },
    .{ .reg = 0x3803, .val = 0x04 },
    .{ .reg = 0x3804, .val = 0x0A },
    .{ .reg = 0x3805, .val = 0x3F },
    .{ .reg = 0x3806, .val = 0x07 },
    .{ .reg = 0x3807, .val = 0x9B },
    .{ .reg = 0x3810, .val = 0x00 },
    .{ .reg = 0x3811, .val = 0x10 },
    .{ .reg = 0x3812, .val = 0x00 },
    .{ .reg = 0x3813, .val = 0x06 },
    .{ .reg = 0x3814, .val = 0x31 },
    .{ .reg = 0x3815, .val = 0x31 },
    .{ .reg = 0x3820, .val = 0x41 },
    .{ .reg = 0x3821, .val = 0x01 },
    .{ .reg = 0x4300, .val = 0x30 },
    .{ .reg = 0x501F, .val = 0x00 },
    .{ .reg = 0x4713, .val = 0x03 },
    .{ .reg = 0x4407, .val = 0x04 },
    .{ .reg = 0x460B, .val = 0x35 },
    .{ .reg = 0x460C, .val = 0x22 },
    .{ .reg = 0x4837, .val = 0x0A },
    .{ .reg = reg.pclk_divider, .val = val.pclk_divider_raw },
    .{ .reg = 0x5000, .val = 0xA7 },
    .{ .reg = 0x5001, .val = 0xA3 },
    .{ .reg = 0x5180, .val = 0xFF },
    .{ .reg = 0x5181, .val = 0xF2 },
    .{ .reg = 0x5182, .val = 0x00 },
    .{ .reg = 0x5183, .val = 0x14 },
    .{ .reg = 0x5184, .val = 0x25 },
    .{ .reg = 0x5185, .val = 0x24 },
    .{ .reg = 0x5186, .val = 0x09 },
    .{ .reg = 0x5187, .val = 0x09 },
    .{ .reg = 0x5188, .val = 0x09 },
    .{ .reg = 0x5189, .val = 0x88 },
    .{ .reg = 0x518A, .val = 0x54 },
    .{ .reg = 0x518B, .val = 0xEE },
    .{ .reg = 0x518C, .val = 0xB2 },
    .{ .reg = 0x518D, .val = 0x50 },
    .{ .reg = 0x518E, .val = 0x34 },
    .{ .reg = 0x518F, .val = 0x6B },
    .{ .reg = 0x5190, .val = 0x46 },
    .{ .reg = 0x5191, .val = 0xF8 },
    .{ .reg = 0x5192, .val = 0x04 },
    .{ .reg = 0x5193, .val = 0x70 },
    .{ .reg = 0x5194, .val = 0xF0 },
    .{ .reg = 0x5195, .val = 0xF0 },
    .{ .reg = 0x5196, .val = 0x03 },
    .{ .reg = 0x5197, .val = 0x01 },
    .{ .reg = 0x5198, .val = 0x04 },
    .{ .reg = 0x5199, .val = 0x6C },
    .{ .reg = 0x519A, .val = 0x04 },
    .{ .reg = 0x519B, .val = 0x00 },
    .{ .reg = 0x519C, .val = 0x09 },
    .{ .reg = 0x519D, .val = 0x2B },
    .{ .reg = 0x519E, .val = 0x38 },
    .{ .reg = 0x5381, .val = 0x1E },
    .{ .reg = 0x5382, .val = 0x5B },
    .{ .reg = 0x5383, .val = 0x08 },
    .{ .reg = 0x5384, .val = 0x0A },
    .{ .reg = 0x5385, .val = 0x7E },
    .{ .reg = 0x5386, .val = 0x88 },
    .{ .reg = 0x5387, .val = 0x7C },
    .{ .reg = 0x5388, .val = 0x6C },
    .{ .reg = 0x5389, .val = 0x10 },
    .{ .reg = 0x538A, .val = 0x01 },
    .{ .reg = 0x538B, .val = 0x98 },
    .{ .reg = 0x5300, .val = 0x08 },
    .{ .reg = 0x5301, .val = 0x30 },
    .{ .reg = 0x5302, .val = 0x10 },
    .{ .reg = 0x5303, .val = 0x00 },
    .{ .reg = 0x5304, .val = 0x08 },
    .{ .reg = 0x5305, .val = 0x30 },
    .{ .reg = 0x5306, .val = 0x08 },
    .{ .reg = 0x5307, .val = 0x16 },
    .{ .reg = 0x5309, .val = 0x08 },
    .{ .reg = 0x530A, .val = 0x30 },
    .{ .reg = 0x530B, .val = 0x04 },
    .{ .reg = 0x530C, .val = 0x06 },
    .{ .reg = 0x5480, .val = 0x01 },
    .{ .reg = 0x5481, .val = 0x08 },
    .{ .reg = 0x5482, .val = 0x14 },
    .{ .reg = 0x5483, .val = 0x28 },
    .{ .reg = 0x5484, .val = 0x51 },
    .{ .reg = 0x5485, .val = 0x65 },
    .{ .reg = 0x5486, .val = 0x71 },
    .{ .reg = 0x5487, .val = 0x7D },
    .{ .reg = 0x5488, .val = 0x87 },
    .{ .reg = 0x5489, .val = 0x91 },
    .{ .reg = 0x548A, .val = 0x9A },
    .{ .reg = 0x548B, .val = 0xAA },
    .{ .reg = 0x548C, .val = 0xB8 },
    .{ .reg = 0x548D, .val = 0xCD },
    .{ .reg = 0x548E, .val = 0xDD },
    .{ .reg = 0x548F, .val = 0xEA },
    .{ .reg = 0x5490, .val = 0x1D },
    .{ .reg = 0x5580, .val = 0x02 },
    .{ .reg = 0x5583, .val = 0x40 },
    .{ .reg = 0x5584, .val = 0x10 },
    .{ .reg = 0x5589, .val = 0x10 },
    .{ .reg = 0x558A, .val = 0x00 },
    .{ .reg = 0x558B, .val = 0xF8 },
    .{ .reg = 0x5800, .val = 0x23 },
    .{ .reg = 0x5801, .val = 0x14 },
    .{ .reg = 0x5802, .val = 0x0F },
    .{ .reg = 0x5803, .val = 0x0F },
    .{ .reg = 0x5804, .val = 0x12 },
    .{ .reg = 0x5805, .val = 0x26 },
    .{ .reg = 0x5806, .val = 0x0C },
    .{ .reg = 0x5807, .val = 0x08 },
    .{ .reg = 0x5808, .val = 0x05 },
    .{ .reg = 0x5809, .val = 0x05 },
    .{ .reg = 0x580A, .val = 0x08 },
    .{ .reg = 0x580B, .val = 0x0D },
    .{ .reg = 0x580C, .val = 0x08 },
    .{ .reg = 0x580D, .val = 0x03 },
    .{ .reg = 0x580E, .val = 0x00 },
    .{ .reg = 0x580F, .val = 0x00 },
    .{ .reg = 0x5810, .val = 0x03 },
    .{ .reg = 0x5811, .val = 0x09 },
    .{ .reg = 0x5812, .val = 0x07 },
    .{ .reg = 0x5813, .val = 0x03 },
    .{ .reg = 0x5814, .val = 0x00 },
    .{ .reg = 0x5815, .val = 0x01 },
    .{ .reg = 0x5816, .val = 0x03 },
    .{ .reg = 0x5817, .val = 0x08 },
    .{ .reg = 0x5818, .val = 0x0D },
    .{ .reg = 0x5819, .val = 0x08 },
    .{ .reg = 0x581A, .val = 0x05 },
    .{ .reg = 0x581B, .val = 0x06 },
    .{ .reg = 0x581C, .val = 0x08 },
    .{ .reg = 0x581D, .val = 0x0E },
    .{ .reg = 0x581E, .val = 0x29 },
    .{ .reg = 0x581F, .val = 0x17 },
    .{ .reg = 0x5820, .val = 0x11 },
    .{ .reg = 0x5821, .val = 0x11 },
    .{ .reg = 0x5822, .val = 0x15 },
    .{ .reg = 0x5823, .val = 0x28 },
    .{ .reg = 0x5824, .val = 0x46 },
    .{ .reg = 0x5825, .val = 0x26 },
    .{ .reg = 0x5826, .val = 0x08 },
    .{ .reg = 0x5827, .val = 0x26 },
    .{ .reg = 0x5828, .val = 0x64 },
    .{ .reg = 0x5829, .val = 0x26 },
    .{ .reg = 0x582A, .val = 0x24 },
    .{ .reg = 0x582B, .val = 0x22 },
    .{ .reg = 0x582C, .val = 0x24 },
    .{ .reg = 0x582D, .val = 0x24 },
    .{ .reg = 0x582E, .val = 0x06 },
    .{ .reg = 0x582F, .val = 0x22 },
    .{ .reg = 0x5830, .val = 0x40 },
    .{ .reg = 0x5831, .val = 0x42 },
    .{ .reg = 0x5832, .val = 0x24 },
    .{ .reg = 0x5833, .val = 0x26 },
    .{ .reg = 0x5834, .val = 0x24 },
    .{ .reg = 0x5835, .val = 0x22 },
    .{ .reg = 0x5836, .val = 0x22 },
    .{ .reg = 0x5837, .val = 0x26 },
    .{ .reg = 0x5838, .val = 0x44 },
    .{ .reg = 0x5839, .val = 0x24 },
    .{ .reg = 0x583A, .val = 0x26 },
    .{ .reg = 0x583B, .val = 0x28 },
    .{ .reg = 0x583C, .val = 0x42 },
    .{ .reg = 0x583D, .val = 0xCE },
    .{ .reg = 0x5025, .val = 0x00 },
    .{ .reg = 0x3000, .val = 0x20 },
    .{ .reg = 0x3002, .val = 0x1C },
    .{ .reg = 0x3004, .val = 0xFF },
    .{ .reg = 0x3006, .val = 0xC3 },
    .{ .reg = 0x300E, .val = 0x58 },
    .{ .reg = 0x302E, .val = 0x00 },
    .{ .reg = 0x440E, .val = 0x00 },
    .{ .reg = 0x4709, .val = 0x02 },
    .{ .reg = 0x470A, .val = 0x00 },
    .{ .reg = 0x470B, .val = 0x00 },
    .{ .reg = 0x471D, .val = 0x00 },
    .{ .reg = 0x4713, .val = 0x03 },
    .{ .reg = 0x471C, .val = 0x50 },
    .{ .reg = 0x4740, .val = 0x20 },
    .{ .reg = 0x4005, .val = 0x1A },
    .{ .reg = 0x3406, .val = 0x00 },
    .{ .reg = 0x3503, .val = 0x00 },
    .{ .reg = 0x3008, .val = 0x02 },
    .{ .reg = 0x4745, .val = 0x00 },
    .{ .reg = 0x4301, .val = 0x01 },
    .{ .reg = 0x503D, .val = 0x00 },
};

/// JPEG overlay writes applied on top of the VGA base scene, in order.
pub const jpeg_writes = [_]RegWrite{
    .{ .reg = reg.jpeg_quality, .val = quant_scale_default },
    .{ .reg = reg.jpeg_mode, .val = val.jpeg_mode_dvp_2 },
    .{ .reg = reg.format, .val = val.format_yuyv },
    .{ .reg = reg.isp_mux, .val = val.isp_mux_yuv },
};

/// Masked readbacks proving the common UYVY scene.
pub const uyvy_expect = [_]RegExpect{
    .{ .reg = reg.format, .mask = val.byte_mask, .value = val.format_yuyv },
    .{ .reg = reg.isp_mux, .mask = val.byte_mask, .value = val.isp_mux_yuv },
    .{ .reg = reg.test_pattern, .mask = val.byte_mask, .value = 0x00 },
};

/// Masked readbacks proving the JPEG-specific scene.
pub const jpeg_expect = [_]RegExpect{
    .{ .reg = reg.system_reset02, .mask = val.jpeg_reset_mask, .value = 0x00 },
    .{ .reg = reg.clock_enable02, .mask = val.jpeg_clock_mask, .value = val.jpeg_clock_mask },
    .{ .reg = reg.timing_tc_reg21, .mask = val.jpeg_enable_mask, .value = val.jpeg_enable_mask },
    .{ .reg = reg.jpeg_quality, .mask = val.jpeg_quant_scale_mask, .value = quant_scale_default },
    .{ .reg = reg.jpeg_mode, .mask = val.jpeg_mode_mask, .value = val.jpeg_mode_dvp_2 },
    .{ .reg = reg.polarity_ctrl00, .mask = val.jpeg_sync_polarity_mask, .value = val.jpeg_sync_polarity },
    .{ .reg = reg.pll_bypass, .mask = val.byte_mask, .value = val.pll_bypass_disabled },
    .{ .reg = reg.pclk_divider, .mask = val.byte_mask, .value = val.pclk_divider_raw },
};

/// Read-modify-write merge: replace only the bits selected by `mask`.
pub fn mergeBits(current: u8, mask: u8, value: u8) u8 {
    return (current & ~mask) | (value & mask);
}

/// Combine the high and low chip-ID bytes into the big-endian identifier.
pub fn combineId(hi: u8, lo: u8) u16 {
    return (@as(u16, hi) << val.hi_byte_shift) | @as(u16, lo);
}

/// Whether a readback satisfies one masked expectation.
pub fn expectationMatches(actual: u8, expectation: RegExpect) bool {
    return (actual & expectation.mask) == expectation.value;
}

/// Whether `scale` is inside the sensor's inclusive 0..63 range.
pub fn quantScaleValid(scale: u8) bool {
    return scale <= quant_scale_max;
}

/// Whether `mode` names a validated register table. Taken as a raw byte: the
/// C enum arrives by value, so an out-of-range byte has to stay rejectable.
pub fn modeSupported(mode: u8) bool {
    return (mode == mode_vga_uyvy) or (mode == mode_vga_jpeg);
}

/// Whether the JPEG-specific verification pass applies to `mode`.
pub fn verifiesJpeg(mode: u8) bool {
    return mode == mode_vga_jpeg;
}

/// System-control byte for the requested streaming state.
pub fn streamValue(enabled: bool) u8 {
    return if (enabled) val.sw_reset_wake else val.sw_standby;
}

/// Whether writing `address` in the base table must be followed by the
/// documented MCU-reset settling delay.
pub fn needsMcuResetDelay(address: u16) bool {
    return address == reg.system_reset00;
}

/// Raw register bytes assembled into one JPEG status snapshot.
pub const JpegStatusRaw = struct {
    length_hi: u8 = 0,
    length_mid: u8 = 0,
    length_lo: u8 = 0,
    overflow: u8 = 0,
    jpeg_input: u8 = 0,
    jpeg_ctrl01: u8 = 0,
    jpeg_header: u8 = 0,
    vfifo_ctrl00: u8 = 0,
    width_hi: u8 = 0,
    width_lo: u8 = 0,
    height_hi: u8 = 0,
    height_lo: u8 = 0,
    href_minimum: u8 = 0,
    timing_ctrl21: u8 = 0,
};

/// `ra8_ov5640_jpeg_status_t`: the caller-owned status snapshot.
pub const JpegStatus = extern struct {
    encoded_bytes: u32 = 0,
    compression_width: u16 = 0,
    compression_height: u16 = 0,
    jpeg_ctrl01: u8 = 0,
    vfifo_ctrl00: u8 = 0,
    href_minimum_blanking: u8 = 0,
    fifo_overflow: bool = false,
    input_is_yuv422: bool = false,
    header_output: bool = false,
    compression_enabled: bool = false,
};

comptime {
    // Header order, 4-byte alignment from the leading uint32_t.
    std.debug.assert(@sizeOf(JpegStatus) == 16);
    std.debug.assert(@offsetOf(JpegStatus, "encoded_bytes") == 0);
    std.debug.assert(@offsetOf(JpegStatus, "compression_width") == 4);
    std.debug.assert(@offsetOf(JpegStatus, "compression_height") == 6);
    std.debug.assert(@offsetOf(JpegStatus, "jpeg_ctrl01") == 8);
    std.debug.assert(@offsetOf(JpegStatus, "vfifo_ctrl00") == 9);
    std.debug.assert(@offsetOf(JpegStatus, "href_minimum_blanking") == 10);
    std.debug.assert(@offsetOf(JpegStatus, "fifo_overflow") == 11);
    std.debug.assert(@offsetOf(JpegStatus, "input_is_yuv422") == 12);
    std.debug.assert(@offsetOf(JpegStatus, "header_output") == 13);
    std.debug.assert(@offsetOf(JpegStatus, "compression_enabled") == 14);
    std.debug.assert(@sizeOf(RegWrite) >= 3);
}

/// Decode raw status bytes into the public snapshot. Pure arithmetic: the
/// three length bytes are big-endian, the geometry pairs are big-endian, and
/// each flag is one documented mask test.
pub fn decodeJpegStatus(raw: JpegStatusRaw) JpegStatus {
    return .{
        .encoded_bytes = (@as(u32, raw.length_hi) << 16) |
            (@as(u32, raw.length_mid) << 8) |
            @as(u32, raw.length_lo),
        .compression_width = (@as(u16, raw.width_hi) << 8) | @as(u16, raw.width_lo),
        .compression_height = (@as(u16, raw.height_hi) << 8) | @as(u16, raw.height_lo),
        .jpeg_ctrl01 = raw.jpeg_ctrl01,
        .vfifo_ctrl00 = raw.vfifo_ctrl00,
        .href_minimum_blanking = raw.href_minimum,
        .fifo_overflow = (raw.overflow & val.jfifo_overflow_mask) != 0,
        .input_is_yuv422 = (raw.jpeg_input & val.jpeg_input_yuv422_mask) != 0,
        .header_output = (raw.jpeg_header & val.jpeg_header_mask) != 0,
        .compression_enabled = (raw.timing_ctrl21 & val.jpeg_enable_mask) != 0,
    };
}
