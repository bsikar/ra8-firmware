//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/ceu_init_regs.zig.

const std = @import("std");
const ceu = @import("ceu_init_regs");

/// Records every write in order.
const Regs = struct {
    offs: [8]usize = @splat(0),
    vals: [8]u32 = @splat(0),
    n: usize = 0,

    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.offs[self.n] = off;
        self.vals[self.n] = v;
        self.n += 1;
    }
};

fn base() ceu.Config {
    return std.mem.zeroInit(ceu.Config, .{ .width_px = 640, .height_px = 480, .bytes_per_pixel = 2 });
}

test "Config matches the 60-byte C layout" {
    try std.testing.expectEqual(@as(usize, 60), @sizeOf(ceu.Config));
    try std.testing.expectEqual(@as(usize, 20), @offsetOf(ceu.Config, "capture_format"));
    try std.testing.expectEqual(@as(usize, 30), @offsetOf(ceu.Config, "edge"));
    try std.testing.expectEqual(@as(usize, 34), @offsetOf(ceu.Config, "byte_swap"));
    try std.testing.expectEqual(@as(usize, 38), @offsetOf(ceu.Config, "scale"));
    try std.testing.expectEqual(@as(usize, 53), @offsetOf(ceu.Config, "low_pass_filter"));
}

test "packCamcr places every field at its HUM bit" {
    var c = base();
    c.hsync_polarity = 1;
    c.vsync_polarity = 1;
    c.capture_format = 1;
    c.input_order = 2;
    c.data_bus = 1;
    c.field_polarity = 1;
    c.edge = .{ .data = 1, .hsync = 1, .vsync = 1, .field = 1 };
    try std.testing.expectEqual(@as(u32, 0x0F01_1213), ceu.packCamcr(&c));
}

test "packCapcr, packCaifr and packCdocr" {
    var c = base();
    c.capture_mode = ceu.capture_continuous;
    c.burst_mode = 2;
    c.frame_drop = 3;
    try std.testing.expectEqual(@as(u32, 0x0321_0000), ceu.packCapcr(&c));
    c.first_field = 1;
    c.one_field_only = true;
    c.interlace = true;
    try std.testing.expectEqual(@as(u32, 0x111), ceu.packCaifr(&c));
    c.byte_swap = .{ .swap_8_bit = true, .swap_16_bit = false, .swap_32_bit = true };
    c.output_format = 3;
    c.bundle_write = true;
    try std.testing.expectEqual(@as(u32, 0x1_0015), ceu.packCdocr(&c));
}

test "packCflcr and packCfszr mask each field" {
    var c = base();
    c.scale = .{ .h_mantissa = 0x1F, .h_fraction = 0x1ABC, .v_mantissa = 0x13, .v_fraction = 0x1DEF, .h_output_clip = 0x1234, .v_output_clip = 0x0567 };
    try std.testing.expectEqual(@as(u32, 0x3DEF_FABC), ceu.packCflcr(&c));
    try std.testing.expectEqual(@as(u32, 0x0567_0234), ceu.packCfszr(&c));
}

test "minStrideBytes prefers the clip, then the capture width, and is 0 for JPEG" {
    var c = base();
    try std.testing.expectEqual(@as(u32, 1280), ceu.minStrideBytes(&c));
    c.x_capture_px = 320;
    try std.testing.expectEqual(@as(u32, 640), ceu.minStrideBytes(&c));
    c.scale.h_output_clip = 100;
    try std.testing.expectEqual(@as(u32, 200), ceu.minStrideBytes(&c));
    c.capture_format = ceu.fmt_data_enable;
    try std.testing.expectEqual(@as(u32, 0), ceu.minStrideBytes(&c));
}

test "programGeometry writes CMCYR, CAMOR and CAPWR, zeroed for JPEG" {
    var c = base();
    c.x_start_px = 8;
    c.y_start_px = 4;
    c.y_capture_lines = 240;
    var r = Regs{};
    ceu.programGeometry(&r, &c);
    try std.testing.expectEqualSlices(usize, &.{ ceu.off_cmcyr, ceu.off_camor, ceu.off_capwr }, r.offs[0..3]);
    try std.testing.expectEqualSlices(u32, &.{ 0x01E0_0280, 0x0004_0008, 0x00F0_0280 }, r.vals[0..3]);
    c.capture_format = ceu.fmt_data_enable;
    r = .{};
    ceu.programGeometry(&r, &c);
    try std.testing.expectEqualSlices(u32, &.{ 0, 0, 0 }, r.vals[0..3]);
}

test "programFormat writes CFLCR, CAIFR, CAPCR, CAMCR in order" {
    var c = base();
    c.interlace = true;
    var r = Regs{};
    ceu.programFormat(&r, &c);
    try std.testing.expectEqualSlices(usize, &.{ ceu.off_cflcr, ceu.off_caifr, ceu.off_capcr, ceu.off_camcr }, r.offs[0..4]);
    try std.testing.expectEqual(@as(u32, 0x100), r.vals[1]);
}

test "programDestination falls back to the minimum stride and sets CLFCR" {
    var c = base();
    c.low_pass_filter = true;
    var r = Regs{};
    ceu.programDestination(&r, &c);
    try std.testing.expectEqual(@as(usize, 6), r.n);
    try std.testing.expectEqualSlices(usize, &.{ ceu.off_cfszr, ceu.off_cdwdr, ceu.off_cfwcr, ceu.off_clfcr, ceu.off_cdocr, ceu.off_cetcr }, r.offs[0..6]);
    try std.testing.expectEqualSlices(u32, &.{ 0, 1280, 0, 1, 0, 0 }, r.vals[0..6]);
    c.dst_stride = 2048;
    r = .{};
    ceu.programDestination(&r, &c);
    try std.testing.expectEqual(@as(u32, 2048), r.vals[1]);
}
