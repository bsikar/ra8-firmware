//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const tdc = @import("canfd_tdc");

test "both channels resolve and a third does not" {
    try std.testing.expectEqual(@as(?usize, 0x4038_0000), tdc.channelBase(0));
    try std.testing.expectEqual(@as(?usize, 0x4038_2000), tdc.channelBase(1));
    try std.testing.expectEqual(@as(?usize, null), tdc.channelBase(2));
}

test "manual mode stamps TDE, TDCOC and the offset" {
    const value = tdc.fdcfgValue(0, .{ .enable = true, .manual = true, .offset = 5 });
    try std.testing.expectEqual(tdc.tde | tdc.tdcoc | (5 << 8), value);
}

test "measured mode stamps TDE and the offset but not TDCOC" {
    const value = tdc.fdcfgValue(0, .{ .enable = true, .manual = false, .offset = 0x7F });
    try std.testing.expectEqual(tdc.tde | (0x7F << 8), value);
}

test "disabling clears the TDC fields and keeps every other bit" {
    const others: u32 = 0xFFFE_00FF;
    const old = others | tdc.tde | tdc.tdcoc | (0x7F << 8);
    try std.testing.expectEqual(others, tdc.fdcfgValue(old, .{ .enable = false, .manual = true, .offset = 9 }));
}

test "re-enabling replaces an earlier offset rather than OR-ing into it" {
    const old = tdc.tde | (0x7F << 8);
    try std.testing.expectEqual(tdc.tde | (1 << 8), tdc.fdcfgValue(old, .{ .enable = true, .manual = false, .offset = 1 }));
}

test "the config struct matches ra8_canfd_tdc_cfg_t" {
    try std.testing.expectEqual(@as(usize, 3), @sizeOf(tdc.Cfg));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(tdc.Cfg, "offset"));
}

test "the guards run in the C order: null cfg, channel, offset" {
    const good: tdc.Cfg = .{ .enable = true, .manual = true, .offset = 127 };
    const big: tdc.Cfg = .{ .enable = true, .manual = true, .offset = 128 };
    try std.testing.expectError(error.NullCfg, tdc.validate(9, null));
    try std.testing.expectError(error.ChannelOutOfRange, tdc.validate(2, &big));
    try std.testing.expectError(error.OffsetTooLarge, tdc.validate(1, &big));
    try std.testing.expectEqual(@as(usize, 0x4038_2000), try tdc.validate(1, &good));
}

test "an offset over the limit is refused even with TDC disabled" {
    const off: tdc.Cfg = .{ .enable = false, .manual = false, .offset = 200 };
    try std.testing.expectError(error.OffsetTooLarge, tdc.validate(0, &off));
}
