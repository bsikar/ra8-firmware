//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/acmphs.zig.

const std = @import("std");
const acmphs = @import("acmphs");

test "Cfg mirrors ra8_acmphs_cfg_t" {
    try std.testing.expectEqual(@as(usize, 5), @sizeOf(acmphs.Cfg));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(acmphs.Cfg, "edge"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(acmphs.Cfg, "invert_out"));
}

test "channelOk bounds channels 0..5" {
    try std.testing.expect(acmphs.channelOk(0));
    try std.testing.expect(acmphs.channelOk(5));
    try std.testing.expect(!acmphs.channelOk(6));
}

test "regAddr steps channels 0x100 apart" {
    try std.testing.expectEqual(@as(usize, 0x40236000), acmphs.regAddr(0, acmphs.off_cmpctl));
    try std.testing.expectEqual(@as(usize, 0x4023650C), acmphs.regAddr(5, acmphs.off_cmpmon));
}

test "mstpId maps MSTPD28..25 and none past channel 3" {
    try std.testing.expectEqual(@as(?u16, 0x031C), acmphs.mstpId(0));
    try std.testing.expectEqual(@as(?u16, 0x0319), acmphs.mstpId(3));
    try std.testing.expectEqual(@as(?u16, null), acmphs.mstpId(4));
}

test "packCtl sets enable, edge, inversion and filter" {
    const plain = acmphs.Cfg{ .ivpsel = 1, .ivrefsel = 2, .edge = 0, .filter_en = false, .invert_out = false };
    try std.testing.expectEqual(@as(u8, 0x80), acmphs.packCtl(plain));
    const all = acmphs.Cfg{ .ivpsel = 1, .ivrefsel = 2, .edge = 3, .filter_en = true, .invert_out = true };
    try std.testing.expectEqual(@as(u8, 0x80 | 0x18 | 0x01 | 0x20), acmphs.packCtl(all));
}

test "levelOf reads CMPMON bit 0" {
    try std.testing.expectEqual(acmphs.level_high, acmphs.levelOf(0x01));
    try std.testing.expectEqual(acmphs.level_low, acmphs.levelOf(0xFE));
}

test "ctl_mask covers every CMPCTL field but CSTEN" {
    try std.testing.expectEqual(@as(u8, 0xFB), acmphs.ctl_mask);
}
