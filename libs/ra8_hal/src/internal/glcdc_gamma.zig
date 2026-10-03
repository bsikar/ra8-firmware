//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GLCDC gamma correction (inc/ra8_glcdc.h, RA8FW-545): the per-channel
//! LUT/AREA tables and the OUT_GAMSW switch. HUM Ch 63 "GLCDC" p 3789-3793.

pub const base: usize = 0x40342000;

/// GAM[0..2] block offsets (red, green, blue), 64 bytes each: LUT[8] then AREA[8].
pub const off_gam = [ch_count]usize{ 0x1300, 0x1340, 0x1380 };
pub const off_area: usize = 0x20;
pub const off_out_gamsw: usize = 0x13D8;

pub const ch_count: u8 = 3;
pub const lut_depth: u8 = 16;
pub const reg_count: usize = lut_depth / 2;

pub const gain_h_shift: u5 = 16;
pub const gain_l_mask: u32 = 0x7FF;
pub const area_h_shift: u5 = 16;
pub const area_l_mask: u32 = 0x3FF;
pub const gamsw_gamon: u32 = 1;

pub const Error = error{ ChannelOutOfRange, BadCount };

/// The GLCDC register window; host tests point `base` at a fake block.
pub const Window = struct {
    base: usize,

    fn reg(w: Window, offset: usize) *volatile u32 {
        return @ptrFromInt(w.base + offset);
    }
};

pub fn validate(channel: u8, count: u8) Error!void {
    if (channel >= ch_count) return error.ChannelOutOfRange;
    if (count != lut_depth) return error.BadCount;
}

/// Pack entries 2i and 2i+1: (hi << shift) | (lo & mask), as the C does.
pub fn pack(table: *const [lut_depth]u16, i: usize, shift: u5, mask: u32) u32 {
    return (@as(u32, table[2 * i]) << shift) | (@as(u32, table[2 * i + 1]) & mask);
}

/// HUM "GAMx_LUTn" p 3790 then "GAMx_AREAn" p 3793. `channel` is validated.
pub fn writeTables(w: Window, channel: u8, gain: *const [lut_depth]u16, threshold: *const [lut_depth]u16) void {
    const block = off_gam[channel];
    for (0..reg_count) |i| w.reg(block + 4 * i).* = pack(gain, i, gain_h_shift, gain_l_mask);
    for (0..reg_count) |i| w.reg(block + off_area + 4 * i).* = pack(threshold, i, area_h_shift, area_l_mask);
}

/// HUM "OUT_GAMSW Gamma Correction Block Switch Register" p 3789.
pub fn setEnable(w: Window, enable: bool) void {
    w.reg(off_out_gamsw).* = if (enable) gamsw_gamon else 0;
}
