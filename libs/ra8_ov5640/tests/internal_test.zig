//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the transport-free OV5640 core: the scene tables, the
//! read-modify-write merge, the chip-ID combine, the masked expectation rule
//! and the JPEG status decode. No bus, no mock, no archive.

const std = @import("std");
const testing = std.testing;
const core = @import("implementation");

test "vga base table keeps the qualified row count" {
    try testing.expectEqual(@as(usize, 271), core.vga_uyvy.len);
}

test "vga base table starts at the clock-select row the C wrote first" {
    try testing.expectEqual(@as(u16, 0x3103), core.vga_uyvy[0].reg);
    try testing.expectEqual(@as(u8, 0x11), core.vga_uyvy[0].val);
}

test "vga base table ends on the test-pattern disable" {
    const last = core.vga_uyvy[core.vga_uyvy.len - 1];
    try testing.expectEqual(core.reg.test_pattern, last.reg);
    try testing.expectEqual(@as(u8, 0x00), last.val);
}

test "vga base table holds exactly one mcu-reset row" {
    var count: usize = 0;
    for (core.vga_uyvy) |row| {
        if (core.needsMcuResetDelay(row.reg)) count += 1;
    }
    try testing.expectEqual(@as(usize, 1), count);
}

test "vga base table programs the qualified pll block" {
    var seen_bit_mode = false;
    var seen_sys_div = false;
    var seen_multiplier = false;
    var seen_pre_div = false;
    for (core.vga_uyvy) |row| {
        if (row.reg == core.reg.pll_bit_mode and row.val == core.val.pll_bit_mode_raw) seen_bit_mode = true;
        if (row.reg == core.reg.pll_sys_div and row.val == core.val.pll_sys_div_raw) seen_sys_div = true;
        if (row.reg == core.reg.pll_multiplier and row.val == core.val.pll_multiplier_raw) seen_multiplier = true;
        if (row.reg == core.reg.pll_pre_div and row.val == core.val.pll_pre_div_raw) seen_pre_div = true;
    }
    try testing.expect(seen_bit_mode and seen_sys_div and seen_multiplier and seen_pre_div);
}

test "vga base table selects the yuyv dvp output format" {
    var format: ?u8 = null;
    for (core.vga_uyvy) |row| {
        if (row.reg == core.reg.format) format = row.val;
    }
    try testing.expectEqual(@as(?u8, core.val.format_yuyv), format);
}

test "every vga row addresses a sensor register above the sccb page base" {
    for (core.vga_uyvy) |row| {
        try testing.expect(row.reg >= 0x3000);
    }
}

test "jpeg overlay writes quality, mode, format and isp mux in order" {
    try testing.expectEqual(@as(usize, 4), core.jpeg_writes.len);
    try testing.expectEqual(core.reg.jpeg_quality, core.jpeg_writes[0].reg);
    try testing.expectEqual(core.quant_scale_default, core.jpeg_writes[0].val);
    try testing.expectEqual(core.reg.jpeg_mode, core.jpeg_writes[1].reg);
    try testing.expectEqual(core.val.jpeg_mode_dvp_2, core.jpeg_writes[1].val);
    try testing.expectEqual(core.reg.format, core.jpeg_writes[2].reg);
    try testing.expectEqual(core.reg.isp_mux, core.jpeg_writes[3].reg);
}

test "uyvy expectations cover format, mux and test pattern with full masks" {
    try testing.expectEqual(@as(usize, 3), core.uyvy_expect.len);
    for (core.uyvy_expect) |expectation| {
        try testing.expectEqual(core.val.byte_mask, expectation.mask);
    }
    try testing.expectEqual(core.reg.format, core.uyvy_expect[0].reg);
    try testing.expectEqual(core.reg.isp_mux, core.uyvy_expect[1].reg);
    try testing.expectEqual(core.reg.test_pattern, core.uyvy_expect[2].reg);
}

test "jpeg expectations cover the eight documented fields" {
    try testing.expectEqual(@as(usize, 8), core.jpeg_expect.len);
    try testing.expectEqual(core.reg.system_reset02, core.jpeg_expect[0].reg);
    try testing.expectEqual(@as(u8, 0x00), core.jpeg_expect[0].value);
    try testing.expectEqual(core.reg.pclk_divider, core.jpeg_expect[7].reg);
    try testing.expectEqual(core.val.pclk_divider_raw, core.jpeg_expect[7].value);
}

test "jpeg expectation values never set a bit outside their own mask" {
    for (core.jpeg_expect) |expectation| {
        try testing.expectEqual(expectation.value, expectation.value & expectation.mask);
    }
}

test "merge replaces only the masked bits" {
    try testing.expectEqual(@as(u8, 0x2C), core.mergeBits(0x0C, 0x20, 0x20));
    try testing.expectEqual(@as(u8, 0x0C), core.mergeBits(0x0C, 0x20, 0x00));
}

test "merge with a full mask replaces the whole byte" {
    try testing.expectEqual(@as(u8, 0x5A), core.mergeBits(0xA5, 0xFF, 0x5A));
}

test "merge with an empty mask keeps the byte" {
    try testing.expectEqual(@as(u8, 0xA5), core.mergeBits(0xA5, 0x00, 0x5A));
}

test "merge ignores value bits outside the mask" {
    try testing.expectEqual(@as(u8, 0x01), core.mergeBits(0x00, 0x03, 0xF1));
}

test "merge is idempotent across the whole quantization field" {
    var scale: u8 = 0;
    while (scale <= core.quant_scale_max) : (scale += 1) {
        const once = core.mergeBits(0xC0, core.val.jpeg_quant_scale_mask, scale);
        try testing.expectEqual(once, core.mergeBits(once, core.val.jpeg_quant_scale_mask, scale));
        try testing.expectEqual(@as(u8, 0xC0), once & 0xC0);
        try testing.expectEqual(scale, once & core.val.jpeg_quant_scale_mask);
    }
}

test "chip id combines the two bytes big-endian" {
    try testing.expectEqual(core.chip_id, core.combineId(0x56, 0x40));
    try testing.expectEqual(@as(u16, 0x0000), core.combineId(0x00, 0x00));
    try testing.expectEqual(@as(u16, 0xFFFF), core.combineId(0xFF, 0xFF));
}

test "chip id combine is injective across every byte pair boundary" {
    try testing.expectEqual(@as(u16, 0x5600), core.combineId(0x56, 0x00));
    try testing.expectEqual(@as(u16, 0x0040), core.combineId(0x00, 0x40));
    try testing.expect(core.combineId(0x56, 0x40) != core.combineId(0x40, 0x56));
}

test "expectation matching compares only masked bits" {
    const expectation = core.RegExpect{ .reg = 0x4407, .mask = 0x3F, .value = 0x0C };
    try testing.expect(core.expectationMatches(0x0C, expectation));
    try testing.expect(core.expectationMatches(0xCC, expectation));
    try testing.expect(!core.expectationMatches(0x0D, expectation));
}

test "expectation with a zero mask matches anything expecting zero" {
    const expectation = core.RegExpect{ .reg = 0x0000, .mask = 0x00, .value = 0x00 };
    try testing.expect(core.expectationMatches(0xFF, expectation));
}

test "quantization scale accepts the documented range and rejects above it" {
    var scale: u8 = 0;
    while (true) {
        try testing.expectEqual(scale <= 0x3F, core.quantScaleValid(scale));
        if (scale == 0xFF) break;
        scale += 1;
    }
}

test "quantization scale bounds match the public presets" {
    try testing.expect(core.quantScaleValid(0x00));
    try testing.expect(core.quantScaleValid(core.quant_scale_default));
    try testing.expect(core.quantScaleValid(core.quant_scale_max));
    try testing.expect(!core.quantScaleValid(core.quant_scale_max + 1));
}

test "only the two validated modes are supported across every byte" {
    var mode: u8 = 0;
    while (true) {
        try testing.expectEqual(mode == 0 or mode == 1, core.modeSupported(mode));
        if (mode == 0xFF) break;
        mode += 1;
    }
}

test "only the jpeg mode takes the jpeg overlay and verification" {
    try testing.expect(!core.verifiesJpeg(core.mode_vga_uyvy));
    try testing.expect(core.verifiesJpeg(core.mode_vga_jpeg));
}

test "stream value selects wake or standby" {
    try testing.expectEqual(core.val.sw_reset_wake, core.streamValue(true));
    try testing.expectEqual(core.val.sw_standby, core.streamValue(false));
}

test "only the system reset00 row triggers the mcu-reset delay" {
    try testing.expect(core.needsMcuResetDelay(core.reg.system_reset00));
    try testing.expect(!core.needsMcuResetDelay(core.reg.system_reset02));
    try testing.expect(!core.needsMcuResetDelay(core.reg.sw_reset));
}

test "probe order is primary then secondary" {
    try testing.expectEqual(@as(usize, 2), core.addresses.len);
    try testing.expectEqual(@as(u8, 0x3C), core.addresses[0]);
    try testing.expectEqual(@as(u8, 0x3D), core.addresses[1]);
}

test "status decode assembles the 24-bit encoded length" {
    const status = core.decodeJpegStatus(.{ .length_hi = 0x01, .length_mid = 0x23, .length_lo = 0x45 });
    try testing.expectEqual(@as(u32, 0x012345), status.encoded_bytes);
}

test "status decode saturates the encoded length at the register width" {
    const status = core.decodeJpegStatus(.{ .length_hi = 0xFF, .length_mid = 0xFF, .length_lo = 0xFF });
    try testing.expectEqual(@as(u32, 0x00FFFFFF), status.encoded_bytes);
}

test "status decode assembles the vga compression geometry" {
    const status = core.decodeJpegStatus(.{
        .width_hi = core.val.jpeg_vga_width_hi,
        .width_lo = core.val.jpeg_vga_width_lo,
        .height_hi = core.val.jpeg_vga_height_hi,
        .height_lo = core.val.jpeg_vga_height_lo,
    });
    try testing.expectEqual(@as(u16, 640), status.compression_width);
    try testing.expectEqual(@as(u16, 480), status.compression_height);
}

test "status decode passes the raw pacing bytes straight through" {
    const status = core.decodeJpegStatus(.{
        .jpeg_ctrl01 = 0x5A,
        .vfifo_ctrl00 = 0xA5,
        .href_minimum = 0x3C,
    });
    try testing.expectEqual(@as(u8, 0x5A), status.jpeg_ctrl01);
    try testing.expectEqual(@as(u8, 0xA5), status.vfifo_ctrl00);
    try testing.expectEqual(@as(u8, 0x3C), status.href_minimum_blanking);
}

test "status decode reads each flag from its own documented mask" {
    const raised = core.decodeJpegStatus(.{
        .overflow = core.val.jfifo_overflow_mask,
        .jpeg_input = core.val.jpeg_input_yuv422_mask,
        .jpeg_header = core.val.jpeg_header_mask,
        .timing_ctrl21 = core.val.jpeg_enable_mask,
    });
    try testing.expect(raised.fifo_overflow);
    try testing.expect(raised.input_is_yuv422);
    try testing.expect(raised.header_output);
    try testing.expect(raised.compression_enabled);

    const clear = core.decodeJpegStatus(.{
        .overflow = ~core.val.jfifo_overflow_mask,
        .jpeg_input = ~core.val.jpeg_input_yuv422_mask,
        .jpeg_header = ~core.val.jpeg_header_mask,
        .timing_ctrl21 = ~core.val.jpeg_enable_mask,
    });
    try testing.expect(!clear.fifo_overflow);
    try testing.expect(!clear.input_is_yuv422);
    try testing.expect(!clear.header_output);
    try testing.expect(!clear.compression_enabled);
}

test "status decode of an all-zero snapshot reports nothing set" {
    const status = core.decodeJpegStatus(.{});
    try testing.expectEqual(@as(u32, 0), status.encoded_bytes);
    try testing.expectEqual(@as(u16, 0), status.compression_width);
    try testing.expectEqual(@as(u16, 0), status.compression_height);
    try testing.expect(!status.fifo_overflow);
    try testing.expect(!status.compression_enabled);
}

test "status flags are independent of each other" {
    const only_overflow = core.decodeJpegStatus(.{ .overflow = 0xFF, .jpeg_input = 0x00 });
    try testing.expect(only_overflow.fifo_overflow);
    try testing.expect(!only_overflow.input_is_yuv422);
}

test "public status layout matches the c header" {
    try testing.expectEqual(@as(usize, 16), @sizeOf(core.JpegStatus));
    try testing.expectEqual(@as(usize, 4), @offsetOf(core.JpegStatus, "compression_width"));
    try testing.expectEqual(@as(usize, 14), @offsetOf(core.JpegStatus, "compression_enabled"));
}

test "settle intervals match the datasheet-derived constants" {
    try testing.expectEqual(@as(u32, 100), core.delay.reset_guard_ms);
    try testing.expectEqual(@as(u32, 10), core.delay.mcu_reset_ms);
    try testing.expectEqual(@as(u32, 5), core.delay.stream_settle_ms);
    try testing.expectEqual(@as(u32, 500), core.delay.cfg_settle_ms);
}

test "software control bytes stay distinct" {
    try testing.expect(core.val.sw_reset_hold != core.val.sw_standby);
    try testing.expect(core.val.sw_standby != core.val.sw_reset_wake);
    try testing.expectEqual(@as(u8, 0x82), core.val.sw_reset_hold);
}
