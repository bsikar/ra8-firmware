//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/glcdc_layer.zig (RA8FW-604).

const std = @import("std");
const g = @import("glcdc_layer");

const Op = struct { off: usize, value: u32 };

const Fake = struct {
    mem: [0x1300 / 4]u32 = [_]u32{0} ** (0x1300 / 4),
    ops: [300]Op = undefined,
    n: usize = 0,
    reads: usize = 0,
    /// When set, BG_EN reads return VEN still set for this many reads.
    ven_sticky: u32 = 0,
    pub fn read32(self: *Fake, off: usize) u32 {
        self.reads += 1;
        if (off == g.off_bg_en and self.ven_sticky > 0) {
            self.ven_sticky -= 1;
            return self.mem[off / 4] | (1 << 8);
        }
        if (off == g.off_bg_en) return self.mem[off / 4] & ~@as(u32, 1 << 8);
        return self.mem[off / 4];
    }
    pub fn write32(self: *Fake, off: usize, value: u32) void {
        if (self.n < self.ops.len) self.ops[self.n] = .{ .off = off, .value = value };
        self.n += 1;
        self.mem[off / 4] = value;
    }
    fn at(self: *const Fake, off: usize) u32 {
        return self.mem[off / 4];
    }
};

const Ctx = struct {
    errs: usize = 0,
    info_value: ?u32 = null,
    pub fn err(self: *Ctx, _: [*:0]const u8) void {
        self.errs += 1;
    }
    pub fn infoVal(self: *Ctx, _: [*:0]const u8, value: u32) void {
        self.info_value = value;
    }
};

fn expectOps(f: *const Fake, want: []const Op) !void {
    try std.testing.expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |w, got| try std.testing.expectEqual(w, got);
}

test "layer2 cfg matches the C layout" {
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(g.Layer2Cfg));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(g.Layer2Cfg, "width_px"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(g.Layer2Cfg, "format"));
    try std.testing.expectEqual(@as(usize, 17), @offsetOf(g.Layer2Cfg, "alpha"));
}

test "set layer2 programs GR2 in the C's order and logs the framebuffer" {
    var f = Fake{};
    var c = Ctx{};
    const cfg = g.Layer2Cfg{ .framebuffer_addr = 0x7000_0000, .line_stride_bytes = 960, .width_px = 480, .height_px = 272, .pos_x = 10, .pos_y = 20, .format = 2, .alpha = 0x80 };
    try std.testing.expectEqual(g.ok, g.setLayer2(&f, &c, &cfg));
    try expectOps(&f, &.{
        .{ .off = g.off_gr2_fmt, .value = 2 },
        .{ .off = g.off_gr2_saddr, .value = 0x7000_0000 },
        .{ .off = g.off_gr2_flm3, .value = 960 << 16 },
        .{ .off = g.off_gr2_line, .value = (272 << 16) | 480 },
        .{ .off = g.off_gr2_size, .value = (10 << 16) | 480 },
        .{ .off = g.off_gr2_ab2, .value = (20 << 16) | 272 },
        .{ .off = g.off_gr2_ab5, .value = (10 << 16) | 480 },
        .{ .off = g.off_gr2_ab4, .value = (20 << 16) | 272 },
        .{ .off = g.off_gr2_ab7, .value = 0x80 << 16 },
        .{ .off = g.off_gr2_ab1, .value = 1 },
        .{ .off = g.off_gr2_flmrd, .value = 1 },
        .{ .off = g.off_gr2_en, .value = 1 },
    });
    try std.testing.expectEqual(@as(?u32, 0x7000_0000), c.info_value);
}

test "set layer2 with a null cfg logs and writes nothing" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(g.null_ptr, g.setLayer2(&f, &c, null));
    try std.testing.expectEqual(@as(usize, 1), c.errs);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "blend modes map to DISPSEL and ARCON" {
    var f = Fake{};
    try std.testing.expectEqual(g.ok, g.setBlend(&f, g.blend_overwrite, 0));
    try std.testing.expectEqual(@as(u32, 1), f.at(g.off_gr1_ab1));
    try std.testing.expectEqual(g.ok, g.setBlend(&f, g.blend_normal, 0));
    try std.testing.expectEqual(@as(u32, 2), f.at(g.off_gr1_ab1));
    try std.testing.expectEqual(g.ok, g.setBlend(&f, g.blend_alpha, 0x40));
    try std.testing.expectEqual(@as(u32, 0x1002), f.at(g.off_gr1_ab1));
    try std.testing.expectEqual(@as(u32, 0x40 << 16), f.at(g.off_gr1_ab7));
    const before = f.n;
    try std.testing.expectEqual(g.invalid_arg, g.setBlend(&f, 3, 0));
    try std.testing.expectEqual(before, f.n);
}

test "background colour waits for VEN to clear before writing BGC" {
    var f = Fake{ .ven_sticky = 3 };
    f.mem[g.off_bg_en / 4] = 0x1;
    try std.testing.expectEqual(g.ok, g.setBackgroundColor(&f, 0xFF11_2233));
    try std.testing.expectEqual(@as(u32, 0x101), f.ops[0].value);
    // One read for the RMW, then three polls: two see VEN set, one sees it clear.
    try std.testing.expectEqual(@as(usize, 4), f.reads);
    try std.testing.expectEqual(@as(u32, 0xFF11_2233), f.at(g.off_bg_bgc));
}

test "background colour gives up after the bounded wait and still writes" {
    var f = Fake{ .ven_sticky = std.math.maxInt(u32) };
    try std.testing.expectEqual(g.ok, g.setBackgroundColor(&f, 7));
    try std.testing.expectEqual(@as(usize, 1 + 0x4_0000), f.reads);
    try std.testing.expectEqual(@as(u32, 7), f.at(g.off_bg_bgc));
}

test "layer1 show enables GR1 opaque on the lower layer" {
    var f = Fake{};
    try std.testing.expectEqual(g.ok, g.layer1Show(&f, 0x6000_0000));
    try expectOps(&f, &.{
        .{ .off = g.off_gr1_saddr, .value = 0x6000_0000 },
        .{ .off = g.off_gr1_ab7, .value = 0xFF << 16 },
        .{ .off = g.off_gr1_ab1, .value = 3 },
        .{ .off = g.off_gr1_flmrd, .value = 1 },
        .{ .off = g.off_gr1_en, .value = 1 },
    });
}

test "chroma key keeps the C's CKON candidates and ORs ARCON into AB1" {
    var f = Fake{};
    f.mem[g.off_gr2_ab1 / 4] = 0x3;
    try std.testing.expectEqual(g.ok, g.layer2ChromaKeyEnable(&f, 0xAB12_3456));
    try std.testing.expectEqual(@as(u32, 0xFF12_3456), f.at(g.off_gr2_ab8));
    try std.testing.expectEqual(@as(u32, 0), f.at(g.off_gr2_ab9));
    try std.testing.expectEqual(@as(u32, 0x01FF_0001), f.at(g.off_gr2_ab7));
    try std.testing.expectEqual(@as(u32, 0x1003), f.at(g.off_gr2_ab1));
    try std.testing.expectEqual(@as(u32, 1), f.at(g.off_gr2_en));
}

test "layer2 show derives DATANUM and LNNUM from the RGB565 framebuffer" {
    var f = Fake{};
    try std.testing.expectEqual(g.ok, g.layer2Show(&f, 0x7010_0000, 32, 48, 320, 240));
    try std.testing.expectEqual(@as(u32, 0x2000_0000), f.at(g.off_gr2_fmt));
    try std.testing.expectEqual(@as(u32, 640 << 16), f.at(g.off_gr2_flm3));
    try std.testing.expectEqual(@as(u32, (239 << 16) | 9), f.at(g.off_gr2_line));
    try std.testing.expectEqual(@as(u32, (32 << 16) | 320), f.at(g.off_gr2_size));
    try std.testing.expectEqual(@as(u32, (48 << 16) | 240), f.at(g.off_gr2_ab4));
    try std.testing.expectEqual(@as(u32, 3), f.at(g.off_gr2_ab1));
    try std.testing.expectEqual(@as(usize, 12), f.n);
}

test "layer2 show wraps like unsigned C for a tiny framebuffer" {
    var f = Fake{};
    try std.testing.expectEqual(g.ok, g.layer2Show(&f, 0, 0, 0, 16, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), f.at(g.off_gr2_line));
}

test "clut fills the inactive plane and swaps on request" {
    var f = Fake{};
    var c = Ctx{};
    const clut = [_]u32{ 0x11, 0x22, 0x33 };
    try std.testing.expectEqual(g.ok, g.setClutDoubleBuffered(&f, &c, 0, &clut, 3, true));
    try std.testing.expectEqual(@as(u32, 0x33), f.at(g.off_gr1_clut1 + 8));
    try std.testing.expectEqual(@as(u32, 0x1_0000), f.at(g.off_gr1_clutint));
    f.mem[g.off_gr2_clutint / 4] = 0x1_00FF;
    try std.testing.expectEqual(g.ok, g.setClutDoubleBuffered(&f, &c, 1, &clut, 3, true));
    try std.testing.expectEqual(@as(u32, 0x11), f.at(g.off_gr2_clut0));
    try std.testing.expectEqual(@as(u32, 0xFF), f.at(g.off_gr2_clutint));
}

test "clut without swap leaves CLUTINT alone" {
    var f = Fake{};
    var c = Ctx{};
    const clut = [_]u32{0xAA} ** 256;
    try std.testing.expectEqual(g.ok, g.setClutDoubleBuffered(&f, &c, 1, &clut, 256, false));
    try std.testing.expectEqual(@as(usize, 256), f.n);
    try std.testing.expectEqual(@as(u32, 0xAA), f.at(g.off_gr2_clut1 + 255 * 4));
}

test "clut rejects null, bad layer and bad entry counts" {
    var f = Fake{};
    var c = Ctx{};
    const clut = [_]u32{0};
    try std.testing.expectEqual(g.null_ptr, g.setClutDoubleBuffered(&f, &c, 0, null, 1, false));
    try std.testing.expectEqual(g.invalid_arg, g.setClutDoubleBuffered(&f, &c, 2, &clut, 1, false));
    try std.testing.expectEqual(g.invalid_arg, g.setClutDoubleBuffered(&f, &c, 0, &clut, 0, false));
    try std.testing.expectEqual(g.invalid_arg, g.setClutDoubleBuffered(&f, &c, 0, &clut, 257, false));
    try std.testing.expectEqual(@as(usize, 1), c.errs);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}
