//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/vreg.zig (RA8FW-788).

const std = @import("std");
const v = @import("vreg");

/// Fake register block that records every DCDCCTL write.
const Fake = struct {
    dcdcctl: u8 = 0,
    log: [8]u8 = undefined,
    n: usize = 0,

    pub fn read(f: *Fake, off: u16) u8 {
        std.debug.assert(off == v.off_dcdcctl);
        return f.dcdcctl;
    }
    pub fn write(f: *Fake, off: u16, value: u8) void {
        std.debug.assert(off == v.off_dcdcctl);
        f.dcdcctl = value;
        f.log[f.n] = value;
        f.n += 1;
    }
    fn writes(f: *const Fake) []const u8 {
        return f.log[0..f.n];
    }
};

test "staged DCDC enable writes STOPZA, PD clear, then 0x10 0x11 0x13" {
    var f = Fake{ .dcdcctl = 0x80 };
    try std.testing.expectEqual(v.step_with_ocp, v.enableDcdc(&f, false));
    try std.testing.expectEqualSlices(u8, &.{ 0x90, 0x10, 0x10, 0x11, 0x13 }, f.writes());
}

test "fast DCDC enable ends at 0x53" {
    var f = Fake{ .dcdcctl = 0x20 };
    try std.testing.expectEqual(v.step_fast_on, v.enableDcdc(&f, true));
    try std.testing.expectEqualSlices(u8, &.{ 0x30, 0x30, 0x53 }, f.writes());
}

test "disable writes 0 or LCBOOST" {
    var f = Fake{ .dcdcctl = 0x13 };
    v.disableDcdc(&f, true);
    v.disableDcdc(&f, false);
    try std.testing.expectEqualSlices(u8, &.{ 0x20, 0x00 }, f.writes());
}

test "validate rejects each out-of-range field" {
    const good = v.Cfg{ .mode = 1, .vccsel = 2, .ocp = 3, .fast_startup = false, .ldo_boost = false, .lv_profile = 2 };
    try std.testing.expectEqual(@as(?u16, null), v.validate(&good));
    var c = good;
    c.mode = 2;
    try std.testing.expectEqual(@as(?u16, v.err_invalid_arg), v.validate(&c));
    c = good;
    c.vccsel = 3;
    try std.testing.expectEqual(@as(?u16, v.err_invalid_arg), v.validate(&c));
    c = good;
    c.ocp = 4;
    try std.testing.expectEqual(@as(?u16, v.err_invalid_arg), v.validate(&c));
    c = good;
    c.lv_profile = 3;
    try std.testing.expectEqual(@as(?u16, v.err_invalid_arg), v.validate(&c));
}

test "LVOCR profile encode and decode" {
    try std.testing.expectEqual(@as(u8, 0x01), v.lvocrOf(v.lv_p0));
    try std.testing.expectEqual(@as(u8, 0x02), v.lvocrOf(v.lv_p1));
    try std.testing.expectEqual(@as(u8, 0), v.lvocrOf(v.lv_off));
    try std.testing.expectEqual(v.lv_p1, v.profileOf(0xFE));
    try std.testing.expectEqual(v.lv_off, v.profileOf(0x03));
}

test "packLdo and OCP decode with the cached level" {
    const c = v.Cfg{ .mode = 0, .vccsel = 0, .ocp = 2, .fast_startup = true, .ldo_boost = true, .lv_profile = 0 };
    try std.testing.expectEqual(@as(u8, 0x62), v.packLdo(&c));
    try std.testing.expectEqual(v.ocp_off, v.ocpOf(0x11, 2));
    try std.testing.expectEqual(@as(u8, 2), v.ocpOf(0x13, 2));
    try std.testing.expectEqual(v.ocp_normal, v.ocpOf(0x13, v.ocp_off));
}

test "decode reads mode, ready and flag bits" {
    const s = v.decode(0x53, 0x06, 0x01, v.ocp_off);
    try std.testing.expectEqual(v.mode_dcdc, s.mode);
    try std.testing.expectEqual(@as(u8, 2), s.vccsel_dec);
    try std.testing.expectEqual(v.lv_p0, s.lv_profile);
    try std.testing.expect(s.dcdc_ready and s.fast_startup and s.io_buf_on and !s.ldo_boost);
    const off = v.decode(0x81, 0, 0, v.ocp_off);
    try std.testing.expect(!off.dcdc_ready);
}
